using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Media.Editing;
using Windows.Storage;
using Windows.Storage.FileProperties;

namespace FernUI.Models
{
    public static class ClipLibraryService
    {
        private sealed class CachedClipEntry
        {
            public string Signature { get; set; } = string.Empty;
            public ClipModel Clip { get; set; } = new();
        }

        private static readonly HashSet<string> VideoExtensions = new(StringComparer.OrdinalIgnoreCase)
        {
            ".mp4",
            ".m4v",
            ".mov",
            ".mkv",
            ".avi",
            ".webm"
        };

        private static readonly object CacheGate = new();
        private static readonly Dictionary<string, CachedClipEntry> ClipCache = new(StringComparer.OrdinalIgnoreCase);
        private static List<ClipModel> CachedOrderedClips = new();

        public static bool IsVideoFile(string path)
        {
            return VideoExtensions.Contains(Path.GetExtension(path)) && !IsFernAudioTempFile(path);
        }

        public static bool IsFernAudioTempFile(string path)
        {
            string fileName = Path.GetFileName(path);
            return fileName.Contains(".fern_audio.tmp", StringComparison.OrdinalIgnoreCase);
        }

        public static void DeleteFernAudioTempFiles()
        {
            string storagePath = SettingsService.NormalizeStoragePath(SettingsService.Instance.StoragePath);

            try
            {
                if (!Directory.Exists(storagePath)) return;

                foreach (string path in Directory.EnumerateFiles(storagePath)
                    .Where(IsFernAudioTempFile))
                {
                    TryDeleteFile(path);
                }
            }
            catch (IOException)
            {
                // Cleanup is best-effort; the gallery filter still hides these files.
            }
            catch (UnauthorizedAccessException)
            {
                // Cleanup is best-effort; the gallery filter still hides these files.
            }
        }

        private static void TryDeleteFile(string path)
        {
            try
            {
                File.Delete(path);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }

        public static List<ClipModel> GetCachedClips()
        {
            lock (CacheGate)
            {
                return CachedOrderedClips.ToList();
            }
        }

        public static async Task<List<ClipModel>> LoadClipsAsync()
        {
            string storagePath = SettingsService.NormalizeStoragePath(SettingsService.Instance.StoragePath);
            Directory.CreateDirectory(storagePath);

            var files = Directory.EnumerateFiles(storagePath)
                .Where(IsVideoFile)
                .Select(path => new FileInfo(path))
                .OrderByDescending(fileInfo => fileInfo.CreationTimeUtc)
                .ToList();

            var clips = new List<ClipModel>(files.Count);
            var livePaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            foreach (FileInfo fileInfo in files)
            {
                fileInfo.Refresh();
                if (!fileInfo.Exists) continue;

                string filePath = fileInfo.FullName;
                string signature;
                try
                {
                    signature = BuildSignature(fileInfo);
                }
                catch (IOException)
                {
                    continue;
                }
                catch (UnauthorizedAccessException)
                {
                    continue;
                }

                livePaths.Add(filePath);

                ClipModel? cachedClip = TryGetCachedClip(filePath, signature);
                if (cachedClip != null)
                {
                    cachedClip.Date = FormatRelativeDate(fileInfo.CreationTime);
                    clips.Add(cachedClip);
                    continue;
                }

                ClipModel clip = await CreateClipMetadataAsync(fileInfo, signature);
                clips.Add(clip);

                lock (CacheGate)
                {
                    ClipCache[filePath] = new CachedClipEntry
                    {
                        Signature = signature,
                        Clip = clip
                    };
                }
            }

            List<ClipModel> orderedClips = clips
                .OrderByDescending(clip => clip.CreationDate)
                .ToList();

            lock (CacheGate)
            {
                foreach (string stalePath in ClipCache.Keys.Where(path => !livePaths.Contains(path)).ToList())
                {
                    ClipCache.Remove(stalePath);
                }

                CachedOrderedClips = orderedClips;
            }

            return orderedClips;
        }

        public static async Task<bool> EnsurePreviewAsync(ClipModel clip, CancellationToken cancellationToken)
        {
            if (clip.PreviewImage != null || string.IsNullOrWhiteSpace(clip.FilePath))
            {
                return false;
            }

            cancellationToken.ThrowIfCancellationRequested();

            try
            {
                StorageFile storageFile = await StorageFile.GetFileFromPathAsync(clip.FilePath);
                cancellationToken.ThrowIfCancellationRequested();

                BitmapImage? previewImage = await LoadPreviewAsync(storageFile, clip.DurationValue);
                cancellationToken.ThrowIfCancellationRequested();

                if (previewImage == null)
                {
                    return false;
                }

                clip.PreviewImage = previewImage;
                return true;
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch
            {
                return false;
            }
        }

        private static ClipModel? TryGetCachedClip(string filePath, string signature)
        {
            lock (CacheGate)
            {
                if (ClipCache.TryGetValue(filePath, out CachedClipEntry? entry) &&
                    entry.Signature == signature)
                {
                    return entry.Clip;
                }
            }

            return null;
        }

        private static async Task<ClipModel> CreateClipMetadataAsync(FileInfo fileInfo, string signature)
        {
            string filePath = fileInfo.FullName;
            DateTime created = fileInfo.CreationTime;

            TimeSpan duration = TimeSpan.Zero;
            uint width = 0;
            uint height = 0;

            try
            {
                StorageFile storageFile = await StorageFile.GetFileFromPathAsync(filePath);

                try
                {
                    var videoProperties = await storageFile.Properties.GetVideoPropertiesAsync();
                    duration = videoProperties.Duration;
                    width = videoProperties.Width;
                    height = videoProperties.Height;
                }
                catch
                {
                    // Keep filesystem metadata even if Windows cannot parse video properties.
                }
            }
            catch
            {
                // Keep filesystem metadata even if Windows cannot create a StorageFile.
            }

            string title = Path.GetFileNameWithoutExtension(filePath);
            string resolution = width > 0 && height > 0 ? $"{width} x {height}" : "";

            return new ClipModel
            {
                Title = title,
                Game = "",
                Date = FormatRelativeDate(created),
                CreationDate = created,
                Duration = duration > TimeSpan.Zero ? FormatDuration(duration) : "",
                DurationValue = duration,
                Resolution = resolution,
                FilePath = filePath,
                VideoUrl = new Uri(filePath).AbsoluteUri,
                CardHeight = CalculateCardHeight(width, height),
                CacheSignature = signature
            };
        }

        private static string BuildSignature(FileInfo fileInfo)
        {
            return $"{fileInfo.LastWriteTimeUtc.Ticks}:{fileInfo.Length}";
        }

        private static async Task<BitmapImage?> LoadPreviewAsync(StorageFile storageFile, TimeSpan duration)
        {
            BitmapImage? framePreview = await LoadFramePreviewAsync(storageFile, duration);
            if (framePreview != null) return framePreview;

            try
            {
                using var thumbnail = await storageFile.GetThumbnailAsync(ThumbnailMode.VideosView, 420);
                if (thumbnail == null || thumbnail.Size == 0) return null;

                var bitmap = new BitmapImage();
                await bitmap.SetSourceAsync(thumbnail);
                return bitmap;
            }
            catch
            {
                return null;
            }
        }

        private static async Task<BitmapImage?> LoadFramePreviewAsync(StorageFile storageFile, TimeSpan duration)
        {
            try
            {
                MediaClip clip = await MediaClip.CreateFromFileAsync(storageFile);
                var composition = new MediaComposition();
                composition.Clips.Add(clip);

                TimeSpan previewTime = duration > TimeSpan.FromSeconds(2)
                    ? TimeSpan.FromSeconds(1)
                    : TimeSpan.Zero;

                using var stream = await composition.GetThumbnailAsync(
                    previewTime,
                    640,
                    360,
                    VideoFramePrecision.NearestFrame);

                if (stream == null || stream.Size == 0) return null;

                var bitmap = new BitmapImage();
                await bitmap.SetSourceAsync(stream);
                return bitmap;
            }
            catch
            {
                return null;
            }
        }

        private static double CalculateCardHeight(uint width, uint height)
        {
            if (width == 0 || height == 0) return 220;

            double ratio = (double)height / width;
            return Math.Clamp(240 * ratio + 110, 190, 320);
        }

        private static string FormatDuration(TimeSpan duration)
        {
            if (duration.TotalHours >= 1)
            {
                return duration.ToString(@"h\:mm\:ss", CultureInfo.InvariantCulture);
            }

            return duration.ToString(@"m\:ss", CultureInfo.InvariantCulture);
        }

        private static string FormatRelativeDate(DateTime created)
        {
            TimeSpan age = DateTime.Now - created;
            if (age.TotalMinutes < 1) return "A l'instant";
            if (age.TotalHours < 1) return $"{(int)age.TotalMinutes} min";
            if (created.Date == DateTime.Today) return "Aujourd'hui";
            if (created.Date == DateTime.Today.AddDays(-1)) return "Hier";
            if (age.TotalDays < 7) return $"{(int)age.TotalDays} j";
            return created.ToString("dd/MM/yyyy", CultureInfo.CurrentCulture);
        }
    }
}
