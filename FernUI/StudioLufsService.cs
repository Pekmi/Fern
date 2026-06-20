using FernUI.Models;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace FernUI
{
    internal sealed class StudioLufsAnalysis
    {
        public double IntegratedLufs { get; init; }
        public double TargetLufs { get; init; }
        public double ErrorLu => IntegratedLufs - TargetLufs;
        public double AbsoluteErrorLu => Math.Abs(ErrorLu);
    }

    internal static class StudioLufsService
    {
        public const double TargetLufs = -14.0;
        public const double TargetTruePeakDb = -1.5;
        public const double TargetLra = 11.0;
        public const double RelativeTolerance = 0.15;

        public static double ToleranceLu => Math.Abs(TargetLufs) * RelativeTolerance;

        public static async Task NormalizeClipAudioAsync(
            string sourcePath,
            IReadOnlyList<AudioTrack> audioTracks,
            StudioLufsAnalysis analysis,
            CancellationToken cancellationToken)
        {
            if (!File.Exists(sourcePath))
            {
                throw new FileNotFoundException("Clip source introuvable.", sourcePath);
            }

            List<AudioTrack> tracks = audioTracks
                .Where(track => track.AudioIndex >= 0)
                .OrderBy(track => track.AudioIndex)
                .ToList();

            if (tracks.Count == 0)
            {
                throw new InvalidOperationException("Aucune piste audio a corriger.");
            }

            double correctionGain = Math.Pow(10.0, (analysis.TargetLufs - analysis.IntegratedLufs) / 20.0);
            if (!double.IsFinite(correctionGain) || correctionGain <= 0)
            {
                throw new InvalidOperationException("Gain LUFS invalide.");
            }

            string directory = Path.GetDirectoryName(sourcePath) ?? string.Empty;
            string stem = Path.GetFileNameWithoutExtension(sourcePath);
            string tempVideoPath = Path.Combine(directory, $"{stem}.lufs.tmp.mp4");
            string packagePath = Path.ChangeExtension(sourcePath, ".fern");
            string tempPackagePath = string.Empty;

            try
            {
                await RunFfmpegAsync(BuildNormalizeVideoArguments(sourcePath, tempVideoPath, tracks, correctionGain), cancellationToken);
                if (File.Exists(packagePath))
                {
                    tempPackagePath = await CreateUpdatedFernPackageAsync(tempVideoPath, packagePath, cancellationToken);
                }

                ReplaceFile(tempVideoPath, sourcePath);
                if (!string.IsNullOrWhiteSpace(tempPackagePath))
                {
                    ReplaceFile(tempPackagePath, packagePath);
                }
            }
            finally
            {
                TryDelete(tempVideoPath);
                TryDelete(tempPackagePath);
            }
        }
        public static async Task ApplyTrackGainsAsync(
            string sourcePath,
            IReadOnlyList<AudioTrack> audioTracks,
            Dictionary<int, double> trackGains,
            CancellationToken cancellationToken)
        {
            if (!File.Exists(sourcePath)) throw new FileNotFoundException("Clip source introuvable.", sourcePath);

            List<AudioTrack> tracks = audioTracks
                .Where(track => track.AudioIndex >= 0)
                .OrderBy(track => track.AudioIndex)
                .ToList();

            if (tracks.Count == 0) throw new InvalidOperationException("Aucune piste audio a modifier.");

            string directory = Path.GetDirectoryName(sourcePath) ?? string.Empty;
            string stem = Path.GetFileNameWithoutExtension(sourcePath);
            string tempVideoPath = Path.Combine(directory, $"{stem}.lufs.tmp.mp4");
            string packagePath = Path.ChangeExtension(sourcePath, ".fern");
            string tempPackagePath = string.Empty;

            try
            {
                await RunFfmpegAsync(BuildTrackGainsVideoArguments(sourcePath, tempVideoPath, tracks, trackGains), cancellationToken);
                if (File.Exists(packagePath)) tempPackagePath = await CreateUpdatedFernPackageAsync(tempVideoPath, packagePath, cancellationToken);
                ReplaceFile(tempVideoPath, sourcePath);
                if (!string.IsNullOrWhiteSpace(tempPackagePath)) ReplaceFile(tempPackagePath, packagePath);
            }
            finally
            {
                TryDelete(tempVideoPath);
                TryDelete(tempPackagePath);
            }
        }

        private static IEnumerable<string> BuildTrackGainsVideoArguments(
            string sourcePath,
            string outputPath,
            IReadOnlyList<AudioTrack> audioTracks,
            Dictionary<int, double> trackGains)
        {
            yield return "-hide_banner";
            yield return "-y";
            yield return "-nostats";
            yield return "-i";
            yield return sourcePath;
            yield return "-filter_complex";

            var filter = new StringBuilder();
            for (int i = 0; i < audioTracks.Count; i++)
            {
                double gain = trackGains.TryGetValue(audioTracks[i].AudioIndex, out double g) ? g : 1.0;
                string gainStr = gain.ToString("0.######", CultureInfo.InvariantCulture);
                filter.Append(CultureInfo.InvariantCulture, $"[0:a:{audioTracks[i].AudioIndex}]volume={gainStr}[norm{i}];");
            }
            if (filter.Length > 0) filter.Length--;
            yield return filter.ToString();

            yield return "-map";
            yield return "0:v:0";

            for (int i = 0; i < audioTracks.Count; i++)
            {
                yield return "-map";
                yield return $"[norm{i}]";
            }

            yield return "-map_metadata";
            yield return "0";
            yield return "-c:v";
            yield return "copy";
            yield return "-c:a";
            yield return "aac";
            yield return "-b:a";
            yield return "192k";
            yield return "-movflags";
            yield return "+faststart";
            yield return outputPath;
        }
        public static async Task<StudioLufsAnalysis?> AnalyzeAsync(
            string sourcePath,
            IReadOnlyList<AudioTrack> audioTracks,
            double masterGain,
            CancellationToken cancellationToken)
        {
            if (!File.Exists(sourcePath)) return null;

            double clampedMasterGain = Math.Clamp(masterGain, 0.0, 1.0);
            List<AudioTrack> measuredTracks = audioTracks
                .Where(track => clampedMasterGain > 0 && track.AudioIndex >= 0)
                .OrderBy(track => track.AudioIndex)
                .ToList();

            if (measuredTracks.Count == 0) return null;

            using var process = new Process();
            process.StartInfo = new ProcessStartInfo
            {
                FileName = "ffmpeg",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };

            foreach (string argument in BuildFfmpegArguments(sourcePath, measuredTracks, clampedMasterGain))
            {
                process.StartInfo.ArgumentList.Add(argument);
            }

            try
            {
                process.Start();
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Studio LUFS: ffmpeg indisponible: {ex.Message}");
                return null;
            }

            Task<string> errorTask = process.StandardError.ReadToEndAsync();
            Task<string> outputTask = process.StandardOutput.ReadToEndAsync();

            try
            {
                await process.WaitForExitAsync(cancellationToken);
            }
            catch (OperationCanceledException)
            {
                TryKill(process);
                throw;
            }

            await outputTask;
            string errorOutput = await errorTask;

            if (process.ExitCode != 0)
            {
                Debug.WriteLine($"Studio LUFS: analyse ffmpeg echouee: {Tail(errorOutput)}");
                return null;
            }

            if (!TryParseInputLufs(errorOutput, out double integratedLufs))
            {
                Debug.WriteLine("Studio LUFS: resultat loudnorm illisible.");
                return null;
            }

            return new StudioLufsAnalysis
            {
                IntegratedLufs = integratedLufs,
                TargetLufs = TargetLufs
            };
        }

        public static async Task<StudioLufsAnalysis?> AnalyzeWithGainsAsync(
            string sourcePath,
            IReadOnlyList<AudioTrack> audioTracks,
            Dictionary<int, double> trackGains,
            CancellationToken cancellationToken)
        {
            if (!File.Exists(sourcePath)) return null;

            List<AudioTrack> measuredTracks = audioTracks
                .Where(track => track.AudioIndex >= 0)
                .OrderBy(track => track.AudioIndex)
                .ToList();

            if (measuredTracks.Count == 0) return null;

            using var process = new Process();
            process.StartInfo = new ProcessStartInfo
            {
                FileName = "ffmpeg",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };

            foreach (string argument in BuildFfmpegArgumentsWithGains(sourcePath, measuredTracks, trackGains))
            {
                process.StartInfo.ArgumentList.Add(argument);
            }

            try
            {
                process.Start();
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Studio LUFS: ffmpeg indisponible: {ex.Message}");
                return null;
            }

            Task<string> errorTask = process.StandardError.ReadToEndAsync();
            Task<string> outputTask = process.StandardOutput.ReadToEndAsync();

            try
            {
                await process.WaitForExitAsync(cancellationToken);
            }
            catch (OperationCanceledException)
            {
                TryKill(process);
                throw;
            }

            await outputTask;
            string errorOutput = await errorTask;

            if (process.ExitCode != 0)
            {
                Debug.WriteLine($"Studio LUFS: analyse ffmpeg echouee: {Tail(errorOutput)}");
                return null;
            }

            if (!TryParseInputLufs(errorOutput, out double integratedLufs))
            {
                Debug.WriteLine("Studio LUFS: resultat loudnorm illisible.");
                return null;
            }

            return new StudioLufsAnalysis
            {
                IntegratedLufs = integratedLufs,
                TargetLufs = TargetLufs
            };
        }

        private static IEnumerable<string> BuildNormalizeVideoArguments(
            string sourcePath,
            string outputPath,
            IReadOnlyList<AudioTrack> audioTracks,
            double correctionGain)
        {
            yield return "-hide_banner";
            yield return "-y";
            yield return "-nostats";
            yield return "-i";
            yield return sourcePath;
            yield return "-filter_complex";
            yield return BuildTrackGainFilter(audioTracks, correctionGain);
            yield return "-map";
            yield return "0:v:0";

            for (int i = 0; i < audioTracks.Count; i++)
            {
                yield return "-map";
                yield return $"[norm{i}]";
            }

            yield return "-map_metadata";
            yield return "0";
            yield return "-c:v";
            yield return "copy";
            yield return "-c:a";
            yield return "aac";
            yield return "-b:a";
            yield return "192k";
            yield return "-movflags";
            yield return "+faststart";
            yield return outputPath;
        }

        private static string BuildTrackGainFilter(IReadOnlyList<AudioTrack> audioTracks, double correctionGain)
        {
            var filter = new StringBuilder();
            string gain = correctionGain.ToString("0.######", CultureInfo.InvariantCulture);

            for (int i = 0; i < audioTracks.Count; i++)
            {
                filter.Append(CultureInfo.InvariantCulture, $"[0:a:{audioTracks[i].AudioIndex}]volume={gain}[norm{i}];");
            }

            if (filter.Length > 0)
            {
                filter.Length--;
            }

            return filter.ToString();
        }

        private static async Task<string> CreateUpdatedFernPackageAsync(
            string normalizedClipPath,
            string packagePath,
            CancellationToken cancellationToken)
        {
            string directory = Path.GetDirectoryName(packagePath) ?? string.Empty;
            string stem = Path.GetFileNameWithoutExtension(packagePath);
            string audioBundlePath = Path.Combine(directory, $"{stem}.lufs.audio.tmp.mp4");
            string tempPackagePath = packagePath + ".lufs.tmp";

            try
            {
                await RunFfmpegAsync(BuildAudioBundleArguments(normalizedClipPath, audioBundlePath), cancellationToken);
                RewritePackageWithAudioBundle(packagePath, tempPackagePath, audioBundlePath);
                return tempPackagePath;
            }
            finally
            {
                TryDelete(audioBundlePath);
            }
        }

        private static IEnumerable<string> BuildAudioBundleArguments(string sourcePath, string outputPath)
        {
            yield return "-hide_banner";
            yield return "-y";
            yield return "-nostats";
            yield return "-i";
            yield return sourcePath;
            yield return "-map";
            yield return "0:a";
            yield return "-c";
            yield return "copy";
            yield return "-movflags";
            yield return "+faststart";
            yield return outputPath;
        }

        private static void RewritePackageWithAudioBundle(string packagePath, string tempPackagePath, string audioBundlePath)
        {
            using var input = File.OpenRead(packagePath);
            Span<byte> magic = stackalloc byte[8];
            if (input.Read(magic) != magic.Length || !magic.SequenceEqual("FERNPKG1"u8))
            {
                throw new InvalidOperationException("Paquet Fern invalide.");
            }

            ulong jsonSize = ReadUInt64(input);
            ulong oldAudioSize = ReadUInt64(input);
            if (jsonSize == 0 || jsonSize > 8 * 1024 * 1024 || oldAudioSize == 0)
            {
                throw new InvalidOperationException("Paquet Fern invalide.");
            }

            byte[] jsonBytes = new byte[(int)jsonSize];
            if (input.Read(jsonBytes, 0, jsonBytes.Length) != jsonBytes.Length)
            {
                throw new EndOfStreamException("Paquet Fern incomplet.");
            }

            using var audio = File.OpenRead(audioBundlePath);
            using var output = File.Open(tempPackagePath, FileMode.Create, FileAccess.Write, FileShare.None);
            output.Write("FERNPKG1"u8);
            WriteUInt64(output, (ulong)jsonBytes.Length);
            WriteUInt64(output, (ulong)audio.Length);
            output.Write(jsonBytes);
            audio.CopyTo(output);
        }

        private static ulong ReadUInt64(Stream stream)
        {
            Span<byte> buffer = stackalloc byte[8];
            if (stream.Read(buffer) != buffer.Length) return 0;
            return BitConverter.ToUInt64(buffer);
        }

        private static void WriteUInt64(Stream stream, ulong value)
        {
            Span<byte> buffer = stackalloc byte[8];
            BitConverter.TryWriteBytes(buffer, value);
            stream.Write(buffer);
        }

        private static async Task RunFfmpegAsync(IEnumerable<string> arguments, CancellationToken cancellationToken)
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

            foreach (string argument in arguments)
            {
                process.StartInfo.ArgumentList.Add(argument);
            }

            try
            {
                process.Start();
            }
            catch (Exception ex)
            {
                throw new InvalidOperationException("FFmpeg est introuvable ou impossible a lancer.", ex);
            }

            Task<string> errorTask = process.StandardError.ReadToEndAsync();
            Task<string> outputTask = process.StandardOutput.ReadToEndAsync();

            try
            {
                await process.WaitForExitAsync(cancellationToken);
            }
            catch (OperationCanceledException)
            {
                TryKill(process);
                throw;
            }

            await outputTask;
            string errorOutput = await errorTask;

            if (process.ExitCode != 0)
            {
                throw new InvalidOperationException($"FFmpeg a echoue: {Tail(errorOutput)}");
            }
        }

        private static IEnumerable<string> BuildFfmpegArguments(
            string sourcePath,
            IReadOnlyList<AudioTrack> audioTracks,
            double masterGain)
        {
            yield return "-hide_banner";
            yield return "-nostats";
            yield return "-i";
            yield return sourcePath;
            yield return "-filter_complex";
            yield return BuildLoudnormFilter(audioTracks, masterGain);
            yield return "-map";
            yield return "[loudout]";
            yield return "-f";
            yield return "null";
            yield return "-";
        }

        private static IEnumerable<string> BuildFfmpegArgumentsWithGains(
            string sourcePath,
            IReadOnlyList<AudioTrack> audioTracks,
            Dictionary<int, double> trackGains)
        {
            yield return "-hide_banner";
            yield return "-nostats";
            yield return "-i";
            yield return sourcePath;
            yield return "-filter_complex";

            var filter = new StringBuilder();
            for (int i = 0; i < audioTracks.Count; i++)
            {
                double gain = trackGains.TryGetValue(audioTracks[i].AudioIndex, out double g) ? g : 1.0;
                string volume = gain.ToString("0.######", CultureInfo.InvariantCulture);
                filter.Append(CultureInfo.InvariantCulture, $"[0:a:{audioTracks[i].AudioIndex}]volume={volume}[a{i}];");
            }

            if (audioTracks.Count == 1)
            {
                filter.Append("[a0]anull[mix];");
            }
            else
            {
                for (int i = 0; i < audioTracks.Count; i++)
                {
                    filter.Append(CultureInfo.InvariantCulture, $"[a{i}]");
                }

                filter.Append(CultureInfo.InvariantCulture,
                    $"amix=inputs={audioTracks.Count}:duration=longest:dropout_transition=0:normalize=0[mix];");
            }

            filter.Append(CultureInfo.InvariantCulture,
                $"[mix]loudnorm=I={TargetLufs:0.##}:TP={TargetTruePeakDb:0.##}:LRA={TargetLra:0.##}:print_format=json[loudout]");

            yield return filter.ToString();
            yield return "-map";
            yield return "[loudout]";
            yield return "-f";
            yield return "null";
            yield return "-";
        }

        private static string BuildLoudnormFilter(IReadOnlyList<AudioTrack> audioTracks, double masterGain)
        {
            var filter = new StringBuilder();
            for (int i = 0; i < audioTracks.Count; i++)
            {
                string volume = Math.Clamp(masterGain, 0.0, 1.0)
                    .ToString("0.######", CultureInfo.InvariantCulture);
                filter.Append(CultureInfo.InvariantCulture, $"[0:a:{audioTracks[i].AudioIndex}]volume={volume}[a{i}];");
            }

            if (audioTracks.Count == 1)
            {
                filter.Append("[a0]anull[mix];");
            }
            else
            {
                for (int i = 0; i < audioTracks.Count; i++)
                {
                    filter.Append(CultureInfo.InvariantCulture, $"[a{i}]");
                }

                filter.Append(CultureInfo.InvariantCulture,
                    $"amix=inputs={audioTracks.Count}:duration=longest:dropout_transition=0:normalize=0[mix];");
            }

            filter.Append(CultureInfo.InvariantCulture,
                $"[mix]loudnorm=I={TargetLufs:0.##}:TP={TargetTruePeakDb:0.##}:LRA={TargetLra:0.##}:print_format=json[loudout]");
            return filter.ToString();
        }

        private static bool TryParseInputLufs(string ffmpegErrorOutput, out double integratedLufs)
        {
            integratedLufs = 0;

            int end = ffmpegErrorOutput.LastIndexOf('}');
            if (end < 0) return false;

            int start = ffmpegErrorOutput.LastIndexOf('{', end);
            if (start < 0 || start >= end) return false;

            string json = ffmpegErrorOutput.Substring(start, end - start + 1);
            try
            {
                using JsonDocument document = JsonDocument.Parse(json);
                if (!document.RootElement.TryGetProperty("input_i", out JsonElement element)) return false;

                string? value = element.GetString();
                if (!double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out integratedLufs))
                {
                    return false;
                }

                return double.IsFinite(integratedLufs);
            }
            catch (JsonException)
            {
                return false;
            }
        }

        private static void ReplaceFile(string sourcePath, string destinationPath)
        {
            if (File.Exists(destinationPath))
            {
                File.Replace(sourcePath, destinationPath, null, ignoreMetadataErrors: true);
                return;
            }

            File.Move(sourcePath, destinationPath);
        }

        private static void TryDelete(string path)
        {
            try
            {
                if (File.Exists(path)) File.Delete(path);
            }
            catch
            {
                // Best-effort cleanup for temporary normalization files.
            }
        }

        private static void TryKill(Process process)
        {
            try
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
            }
            catch
            {
                // Best-effort cleanup for canceled analyses.
            }
        }

        private static string Tail(string value)
        {
            if (string.IsNullOrWhiteSpace(value)) return string.Empty;
            string[] lines = value.Split(new[] { "\r\n", "\n" }, StringSplitOptions.None);
            return string.Join(Environment.NewLine, lines.TakeLast(8));
        }
    }
}
