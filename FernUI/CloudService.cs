using System;
using System.IO;
using System.Net.Http;
using System.Threading.Tasks;
using System.Text.Json;
using FernUI.Models;

namespace FernUI
{
    internal static class CloudService
    {
        private static readonly HttpClient _httpClient = new HttpClient();

        public static async Task<string> UploadClipAsync(string filePath)
        {
            var settings = SettingsService.Instance;
            if (string.IsNullOrEmpty(settings.CloudUrl) || string.IsNullOrEmpty(settings.CloudApiKey))
            {
                throw new InvalidOperationException("Configuration Cloud manquante.");
            }

            using var content = new MultipartFormDataContent();
            using var fileStream = File.OpenRead(filePath);
            content.Add(new StreamContent(fileStream), "clip", Path.GetFileName(filePath));

            using var request = new HttpRequestMessage(HttpMethod.Post, $"{settings.CloudUrl}/upload");
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
            return result.Link;
        }

        private class UploadResponse
        {
            public string? Link { get; set; }
        }
    }
}