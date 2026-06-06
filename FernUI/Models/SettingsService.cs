using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;

namespace FernUI.Models
{
    public class SettingsService
    {
        private static SettingsService? _instance;
        public static SettingsService Instance => _instance ??= new SettingsService();

        public int BufferDuration { get; set; } = 30;
        public int FPS { get; set; } = 60;
        public int Bitrate { get; set; } = 15;
        public string StoragePath { get; set; } = "";
        public string Hotkey { get; set; } = "Alt+Shift+F9";
        public string MicrophoneDeviceId { get; set; } = "";
        public string MicrophoneDeviceName { get; set; } = "";

        private readonly string _filePath;

        private SettingsService()
        {
            string appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            string folder = Path.Combine(appData, "PekmisIndustries", "Fern");
            Directory.CreateDirectory(folder);
            _filePath = Path.Combine(folder, "settings.txt");

            string videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
            StoragePath = NormalizeStoragePath(videos);

            Load();
        }

        public static string NormalizeStoragePath(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                string videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
                path = videos;
            }

            string fullPath = Path.GetFullPath(Environment.ExpandEnvironmentVariables(path.Trim()));
            string leaf = Path.GetFileName(fullPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));

            if (!string.Equals(leaf, "Fern", StringComparison.OrdinalIgnoreCase))
            {
                fullPath = Path.Combine(fullPath, "Fern");
            }

            return fullPath;
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
                bool sawHotkey = false;
                bool sawMicrophoneDeviceId = false;
                bool sawMicrophoneDeviceName = false;
                string loadedStoragePath = StoragePath;

                foreach (var line in File.ReadAllLines(_filePath))
                {
                    int separator = line.IndexOf('=');
                    if (separator <= 0) continue;

                    string key = line[..separator];
                    string value = line[(separator + 1)..].TrimEnd('\r');

                    switch (key)
                    {
                        case "BufferDuration":
                            if (int.TryParse(value, out int bd)) BufferDuration = bd;
                            break;
                        case "FPS":
                            if (int.TryParse(value, out int fps)) FPS = fps;
                            break;
                        case "Bitrate":
                            if (int.TryParse(value, out int br)) Bitrate = br;
                            break;
                        case "StoragePath":
                            StoragePath = value;
                            loadedStoragePath = value;
                            break;
                        case "Hotkey":
                            Hotkey = value;
                            sawHotkey = true;
                            break;
                        case "MicrophoneDeviceId":
                            MicrophoneDeviceId = value;
                            sawMicrophoneDeviceId = true;
                            break;
                        case "MicrophoneDeviceName":
                            MicrophoneDeviceName = value;
                            sawMicrophoneDeviceName = true;
                            break;
                    }
                }

                StoragePath = NormalizeStoragePath(StoragePath);
                if (string.IsNullOrWhiteSpace(Hotkey)) Hotkey = "Alt+Shift+F9";

                if (!sawHotkey || !sawMicrophoneDeviceId || !sawMicrophoneDeviceName ||
                    !string.Equals(loadedStoragePath, StoragePath, StringComparison.OrdinalIgnoreCase))
                {
                    Save();
                }
            }
            catch (Exception)
            {
                StoragePath = NormalizeStoragePath(StoragePath);
                if (string.IsNullOrWhiteSpace(Hotkey)) Hotkey = "Alt+Shift+F9";
            }
        }

        public void Save()
        {
            try
            {
                StoragePath = NormalizeStoragePath(StoragePath);
                if (string.IsNullOrWhiteSpace(Hotkey)) Hotkey = "Alt+Shift+F9";

                var lines = new[]
                {
                    $"BufferDuration={BufferDuration}",
                    $"FPS={FPS}",
                    $"Bitrate={Bitrate}",
                    $"StoragePath={StoragePath}",
                    $"Hotkey={Hotkey}",
                    $"MicrophoneDeviceId={MicrophoneDeviceId}",
                    $"MicrophoneDeviceName={MicrophoneDeviceName}"
                };
                File.WriteAllLines(_filePath, lines);
            }
            catch (Exception)
            {
                // Settings must never prevent the UI from opening.
            }
        }

        public static Task MoveStorageContentsAsync(string sourcePath, string destinationPath)
        {
            return Task.Run(() =>
            {
                sourcePath = NormalizeStoragePath(sourcePath);
                destinationPath = NormalizeStoragePath(destinationPath);

                if (!Directory.Exists(sourcePath) ||
                    string.Equals(sourcePath, destinationPath, StringComparison.OrdinalIgnoreCase))
                {
                    Directory.CreateDirectory(destinationPath);
                    return;
                }

                Directory.CreateDirectory(destinationPath);

                foreach (string file in Directory.EnumerateFiles(sourcePath))
                {
                    string destinationFile = Path.Combine(destinationPath, Path.GetFileName(file));
                    File.Move(file, destinationFile, overwrite: true);
                }

                foreach (string directory in Directory.EnumerateDirectories(sourcePath))
                {
                    string destinationDirectory = Path.Combine(destinationPath, Path.GetFileName(directory));
                    MoveDirectory(directory, destinationDirectory);
                }

                if (!Directory.EnumerateFileSystemEntries(sourcePath).Any())
                {
                    Directory.Delete(sourcePath);
                }
            });
        }

        private static void MoveDirectory(string sourceDirectory, string destinationDirectory)
        {
            if (!Directory.Exists(destinationDirectory))
            {
                Directory.Move(sourceDirectory, destinationDirectory);
                return;
            }

            foreach (string file in Directory.EnumerateFiles(sourceDirectory))
            {
                string destinationFile = Path.Combine(destinationDirectory, Path.GetFileName(file));
                File.Move(file, destinationFile, overwrite: true);
            }

            foreach (string directory in Directory.EnumerateDirectories(sourceDirectory))
            {
                string childDestination = Path.Combine(destinationDirectory, Path.GetFileName(directory));
                MoveDirectory(directory, childDestination);
            }

            Directory.Delete(sourceDirectory);
        }
    }
}
