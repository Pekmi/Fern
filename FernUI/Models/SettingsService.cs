using System;
using System.IO;

namespace FernUI.Models
{
    public class SettingsService
    {
        private static SettingsService _instance;
        public static SettingsService Instance => _instance ??= new SettingsService();

        public int BufferDuration { get; set; } = 30;
        public int FPS { get; set; } = 60;
        public int Bitrate { get; set; } = 15;
        public string StoragePath { get; set; } = "";

        private readonly string _filePath;

        private SettingsService()
        {
            string appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            string folder = Path.Combine(appData, "PekmisIndustries", "Fern");
            Directory.CreateDirectory(folder);
            _filePath = Path.Combine(folder, "settings.txt");

            // Default StoragePath to User's Videos folder
            string videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
            StoragePath = Path.Combine(videos, "Fern");

            Load();
        }

        public void Load()
        {
            if (!File.Exists(_filePath))
            {
                Save();
                return;
            }

            try
            {
                var lines = File.ReadAllLines(_filePath);
                foreach (var line in lines)
                {
                    var parts = line.Split('=');
                    if (parts.Length == 2)
                    {
                        switch (parts[0])
                        {
                            case "BufferDuration":
                                if (int.TryParse(parts[1], out int bd)) BufferDuration = bd;
                                break;
                            case "FPS":
                                if (int.TryParse(parts[1], out int fps)) FPS = fps;
                                break;
                            case "Bitrate":
                                if (int.TryParse(parts[1], out int br)) Bitrate = br;
                                break;
                            case "StoragePath":
                                StoragePath = parts[1];
                                break;
                        }
                    }
                }
            }
            catch (Exception)
            {
                // Fallback to defaults
            }
        }

        public void Save()
        {
            try
            {
                var lines = new[]
                {
                    $"BufferDuration={BufferDuration}",
                    $"FPS={FPS}",
                    $"Bitrate={Bitrate}",
                    $"StoragePath={StoragePath}"
                };
                File.WriteAllLines(_filePath, lines);
            }
            catch (Exception)
            {
                // Ignore save errors for now
            }
        }
    }
}
