using FernUI.Models;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Windows.Storage;

namespace FernUI
{
    internal static class StudioExportService
    {
        private const int AudioBitrateKbps = 192;
        private const int MinimumVideoBitrateKbps = 350;
        private const int MaximumVideoBitrateKbps = 60_000;

        public static async Task<StorageFile> ExportAsync(
            string sourcePath,
            IReadOnlyList<AudioTrack> audioTracks,
            double targetSizeMb,
            TimeSpan duration,
            double masterVolume,
            IProgress<double>? progress)
        {
            if (!File.Exists(sourcePath))
            {
                throw new FileNotFoundException("Clip source introuvable.", sourcePath);
            }

            long sourceBytes = new FileInfo(sourcePath).Length;
            long targetBytes = (long)(Math.Max(0.1, targetSizeMb) * 1024.0 * 1024.0);
            bool encodeVideo = targetBytes < sourceBytes * 0.98;
            double bitrateScale = 1.0;

            for (int attempt = 0; attempt < 3; attempt++)
            {
                StorageFile outputFile = await CreateOutputFileAsync(sourcePath);
                int? videoBitrateKbps = encodeVideo
                    ? CalculateVideoBitrateKbps(targetSizeMb, duration, bitrateScale)
                    : null;

                try
                {
                    await RunFfmpegAsync(
                        sourcePath,
                        outputFile.Path,
                        audioTracks,
                        videoBitrateKbps,
                        masterVolume,
                        duration,
                        progress);
                }
                catch
                {
                    await DeleteIfExistsAsync(outputFile);
                    throw;
                }

                long outputBytes = new FileInfo(outputFile.Path).Length;
                if (outputBytes <= targetBytes || attempt == 2)
                {
                    progress?.Report(100);
                    return outputFile;
                }

                await DeleteIfExistsAsync(outputFile);
                encodeVideo = true;
                bitrateScale *= Math.Clamp((double)targetBytes / outputBytes * 0.90, 0.35, 0.85);
            }

            throw new InvalidOperationException("Export impossible.");
        }

        private static async Task RunFfmpegAsync(
            string sourcePath,
            string outputPath,
            IReadOnlyList<AudioTrack> audioTracks,
            int? videoBitrateKbps,
            double masterVolume,
            TimeSpan duration,
            IProgress<double>? progress)
        {
            using var process = new Process();
            process.StartInfo = new ProcessStartInfo
            {
                FileName = "ffmpeg",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };

            foreach (string argument in BuildFfmpegArguments(sourcePath, outputPath, audioTracks, videoBitrateKbps, masterVolume))
            {
                process.StartInfo.ArgumentList.Add(argument);
            }

            try
            {
                process.Start();
                progress?.Report(0);
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException("FFmpeg est introuvable ou impossible a lancer.", ex);
            }

            Task progressTask = ReadProgressAsync(process.StandardOutput, duration, progress);
            Task<string> errorTask = ReadErrorTailAsync(process.StandardError);

            await process.WaitForExitAsync();
            await progressTask;
            string errorOutput = await errorTask;

            if (process.ExitCode != 0)
            {
                string details = string.IsNullOrWhiteSpace(errorOutput)
                    ? $"code {process.ExitCode}"
                    : errorOutput.Trim();
                throw new InvalidOperationException($"FFmpeg a echoue: {details}");
            }
        }

        private static IEnumerable<string> BuildFfmpegArguments(
            string sourcePath,
            string outputPath,
            IReadOnlyList<AudioTrack> audioTracks,
            int? videoBitrateKbps,
            double masterVolume)
        {
            yield return "-hide_banner";
            yield return "-y";
            yield return "-nostats";
            yield return "-progress";
            yield return "pipe:1";
            yield return "-i";
            yield return sourcePath;

            double clampedMasterVolume = Math.Clamp(masterVolume, 0.0, 1.0);
            List<AudioTrack> audibleTracks = audioTracks
                .Where(track => clampedMasterVolume > 0 && track.AudioIndex >= 0 && track.Volume > 0)
                .OrderBy(track => track.AudioIndex)
                .ToList();

            if (audibleTracks.Count > 0)
            {
                yield return "-filter_complex";
                yield return BuildAudioFilter(audibleTracks, clampedMasterVolume);
            }

            yield return "-map";
            yield return "0:v:0";

            if (audibleTracks.Count > 0)
            {
                yield return "-map";
                yield return "[aout]";
            }
            else
            {
                yield return "-an";
            }

            if (videoBitrateKbps.HasValue)
            {
                int bitrate = videoBitrateKbps.Value;
                yield return "-c:v";
                yield return "libx264";
                yield return "-preset";
                yield return "superfast";
                yield return "-b:v";
                yield return $"{bitrate}k";
                yield return "-maxrate";
                yield return $"{bitrate}k";
                yield return "-bufsize";
                yield return $"{bitrate * 2}k";
                yield return "-pix_fmt";
                yield return "yuv420p";
            }
            else
            {
                yield return "-c:v";
                yield return "copy";
            }

            if (audibleTracks.Count > 0)
            {
                yield return "-c:a";
                yield return "aac";
                yield return "-b:a";
                yield return $"{AudioBitrateKbps}k";
                yield return "-ac";
                yield return "2";
            }

            yield return "-movflags";
            yield return "+faststart";
            yield return outputPath;
        }

        private static string BuildAudioFilter(IReadOnlyList<AudioTrack> audioTracks, double masterVolume)
        {
            var filter = new StringBuilder();
            for (int i = 0; i < audioTracks.Count; i++)
            {
                string volume = Math.Clamp((audioTracks[i].Volume / 100.0) * masterVolume, 0.0, 1.0)
                    .ToString("0.######", CultureInfo.InvariantCulture);
                filter.Append(CultureInfo.InvariantCulture, $"[0:a:{audioTracks[i].AudioIndex}]volume={volume}[a{i}];");
            }

            if (audioTracks.Count == 1)
            {
                filter.Append("[a0]anull[aout]");
                return filter.ToString();
            }

            for (int i = 0; i < audioTracks.Count; i++)
            {
                filter.Append(CultureInfo.InvariantCulture, $"[a{i}]");
            }

            filter.Append(CultureInfo.InvariantCulture,
                $"amix=inputs={audioTracks.Count}:duration=longest:dropout_transition=0:normalize=0[aout]");
            return filter.ToString();
        }

        private static async Task ReadProgressAsync(
            StreamReader reader,
            TimeSpan duration,
            IProgress<double>? progress)
        {
            double durationMicroseconds = Math.Max(1, duration.TotalMilliseconds * 1000.0);

            while (true)
            {
                string? line = await reader.ReadLineAsync();
                if (line == null) break;

                if (line.StartsWith("out_time_us=", StringComparison.OrdinalIgnoreCase) &&
                    long.TryParse(line["out_time_us=".Length..], NumberStyles.Integer, CultureInfo.InvariantCulture, out long timeUs))
                {
                    progress?.Report(Math.Clamp(timeUs / durationMicroseconds * 100.0, 0, 99));
                }
                else if (line.Equals("progress=end", StringComparison.OrdinalIgnoreCase))
                {
                    progress?.Report(100);
                }
            }
        }

        private static async Task<string> ReadErrorTailAsync(StreamReader reader)
        {
            var tail = new Queue<string>();

            while (true)
            {
                string? line = await reader.ReadLineAsync();
                if (line == null) break;

                tail.Enqueue(line);
                while (tail.Count > 12)
                {
                    tail.Dequeue();
                }
            }

            return string.Join(Environment.NewLine, tail);
        }

        private static int CalculateVideoBitrateKbps(double targetSizeMb, TimeSpan duration, double bitrateScale)
        {
            double seconds = Math.Max(1.0, duration.TotalSeconds);
            double targetBytes = Math.Max(1.0, targetSizeMb) * 1024.0 * 1024.0;
            double totalKbps = targetBytes * 8.0 * 0.92 / seconds / 1000.0;
            double videoKbps = (totalKbps - AudioBitrateKbps) * Math.Clamp(bitrateScale, 0.1, 1.0);
            return (int)Math.Clamp(videoKbps, MinimumVideoBitrateKbps, MaximumVideoBitrateKbps);
        }

        private static async Task<StorageFile> CreateOutputFileAsync(string sourcePath)
        {
            string exportDirectory = Path.Combine(Path.GetTempPath(), "FernUI", "Exports");
            Directory.CreateDirectory(exportDirectory);

            StorageFolder exportFolder = await StorageFolder.GetFolderFromPathAsync(exportDirectory);
            string baseName = Path.GetFileNameWithoutExtension(sourcePath);
            string fileName = $"{baseName}_export_{DateTime.Now:yyyyMMdd_HHmmss}.mp4";
            return await exportFolder.CreateFileAsync(fileName, CreationCollisionOption.GenerateUniqueName);
        }

        private static async Task DeleteIfExistsAsync(StorageFile file)
        {
            try
            {
                await file.DeleteAsync(StorageDeleteOption.PermanentDelete);
            }
            catch
            {
                // Best-effort cleanup for failed export attempts.
            }
        }
    }
}
