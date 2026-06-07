using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Threading.Tasks;
using System.Text.Json;
using FernUI.Models;

namespace FernUI.Models
{
    public class CloudClip
    {
        public string Id { get; set; } = string.Empty;
        public string Link { get; set; } = string.Empty;
        public string Name { get; set; } = string.Empty;
        public string Title { get; set; } = string.Empty;
        public DateTime Timestamp { get; set; }
        public long Size { get; set; }

        public string DisplayName
        {
            get
            {
                if (!string.IsNullOrWhiteSpace(Title)) return Title;
                if (!string.IsNullOrWhiteSpace(Name)) return Path.GetFileNameWithoutExtension(Name);
                return $"Clip {Id}";
            }
        }

        public string ExpirationText
        {
            get
            {
                var remaining = Timestamp.AddDays(7) - DateTime.Now;
                if (remaining.TotalDays > 1) return $"Expire dans {Math.Ceiling(remaining.TotalDays)} jours";
                if (remaining.TotalHours > 1) return $"Expire dans {Math.Ceiling(remaining.TotalHours)} heures";
                return "Expire bientôt";
            }
        }
    }
}

namespace FernUI
{
    public static class CloudService
    {
        private static readonly HttpClient _httpClient = new HttpClient();

        public static Task<string> UploadClipAsync(string filePath)
        {
            return UploadClipAsync(filePath, Path.GetFileNameWithoutExtension(filePath));
        }

        public static Task<string> UploadClipAsync(string filePath, string? title)
        {
            return UploadClipAsync(filePath, title, null);
        }

        public static async Task<string> UploadClipAsync(string filePath, string? title, IProgress<double>? progress)
        {
            var settings = SettingsService.Instance;
            if (string.IsNullOrEmpty(settings.CloudUrl) || string.IsNullOrEmpty(settings.CloudApiKey))
            {
                throw new InvalidOperationException("Configuration Cloud manquante.");
            }

            using var content = new MultipartFormDataContent();
            using var fileStream = File.OpenRead(filePath);
            progress?.Report(0);
            content.Add(new ProgressableStreamContent(fileStream, progress), "clip", Path.GetFileName(filePath));
            content.Add(new StringContent(string.IsNullOrWhiteSpace(title) ? Path.GetFileNameWithoutExtension(filePath) : title), "title");

            using var request = new HttpRequestMessage(HttpMethod.Post, BuildCloudUrl("/upload"));
            request.Headers.Add("X-Fern-API-Key", settings.CloudApiKey);
            request.Content = content;

            var response = await _httpClient.SendAsync(request);
            response.EnsureSuccessStatusCode();

            var jsonString = await response.Content.ReadAsStringAsync();
            var result = JsonSerializer.Deserialize<UploadResponse>(jsonString, new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
            
            if (result == null || string.IsNullOrEmpty(result.Link))
            {
                throw new InvalidOperationException("La reponse du serveur est invalide.");
            }
            progress?.Report(100);
            return result.Link;
        }

        public static async Task<List<CloudClip>> GetCloudClipsAsync()
        {
            var settings = SettingsService.Instance;
            if (string.IsNullOrEmpty(settings.CloudUrl) || string.IsNullOrEmpty(settings.CloudApiKey))
            {
                return new List<CloudClip>();
            }

            using var request = new HttpRequestMessage(HttpMethod.Get, BuildCloudUrl("/list"));
            request.Headers.Add("X-Fern-API-Key", settings.CloudApiKey);

            var response = await _httpClient.SendAsync(request);
            if (!response.IsSuccessStatusCode) return new List<CloudClip>();

            var jsonString = await response.Content.ReadAsStringAsync();
            return JsonSerializer.Deserialize<List<CloudClip>>(jsonString, new JsonSerializerOptions { PropertyNameCaseInsensitive = true }) ?? new List<CloudClip>();
        }

        public static async Task<bool> DeleteCloudClipAsync(string id)
        {
            var settings = SettingsService.Instance;
            if (string.IsNullOrEmpty(settings.CloudUrl) ||
                string.IsNullOrEmpty(settings.CloudApiKey) ||
                string.IsNullOrWhiteSpace(id))
            {
                return false;
            }

            using var request = new HttpRequestMessage(HttpMethod.Delete, BuildCloudUrl($"/clip/{Uri.EscapeDataString(id)}"));
            request.Headers.Add("X-Fern-API-Key", settings.CloudApiKey);

            var response = await _httpClient.SendAsync(request);
            return response.IsSuccessStatusCode;
        }

        private static string BuildCloudUrl(string path)
        {
            var baseUrl = SettingsService.Instance.CloudUrl.TrimEnd('/');
            return $"{baseUrl}/{path.TrimStart('/')}";
        }

        private class UploadResponse
        {
            public string? Link { get; set; }
        }

        private sealed class ProgressableStreamContent : HttpContent
        {
            private const int BufferSize = 64 * 1024;
            private readonly Stream _source;
            private readonly IProgress<double>? _progress;
            private readonly long _length;

            public ProgressableStreamContent(Stream source, IProgress<double>? progress)
            {
                _source = source;
                _progress = progress;
                _length = source.CanSeek ? source.Length : -1;
            }

            protected override async Task SerializeToStreamAsync(Stream stream, TransportContext? context)
            {
                var buffer = new byte[BufferSize];
                long uploaded = 0;

                while (true)
                {
                    int read = await _source.ReadAsync(buffer.AsMemory(0, buffer.Length));
                    if (read == 0) break;

                    await stream.WriteAsync(buffer.AsMemory(0, read));
                    uploaded += read;

                    if (_length > 0)
                    {
                        _progress?.Report(Math.Clamp(uploaded * 100.0 / _length, 0, 100));
                    }
                }
            }

            protected override bool TryComputeLength(out long length)
            {
                length = _length;
                return _length >= 0;
            }
        }
    }
}
