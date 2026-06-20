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
        public string VideoCodec { get; set; } = "H264";
        public string EncoderProfile { get; set; } = "High";
        public string RateControl { get; set; } = "VBR";
        public int MaxBitrateMultiplier { get; set; } = 200;
        public int GopSeconds { get; set; } = 2;
        public int BFrames { get; set; } = 2;
        public bool LowLatency { get; set; } = false;
        public int QualityVsSpeed { get; set; } = 70;
        public int EncoderIndex { get; set; } = 0;
        public string CloudUrl { get; set; } = "";
        public string CloudApiKey { get; set; } = "";
        public string TargetScreenName { get; set; } = "";
        public bool IsAiAutoSortEnabled { get; set; } = false;

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
                bool sawVideoCodec = false;
                bool sawEncoderProfile = false;
                bool sawRateControl = false;
                bool sawMaxBitrateMultiplier = false;
                bool sawGopSeconds = false;
                bool sawBFrames = false;
                bool sawLowLatency = false;
                bool sawQualityVsSpeed = false;
                bool sawEncoderIndex = false;
                bool sawCloudUrl = false;
                bool sawCloudApiKey = false;
                bool sawTargetScreenName = false;
                bool sawIsAiAutoSortEnabled = false;
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
                        case "VideoCodec":
                            VideoCodec = value;
                            sawVideoCodec = true;
                            break;
                        case "EncoderProfile":
                            EncoderProfile = value;
                            sawEncoderProfile = true;
                            break;
                        case "RateControl":
                            RateControl = value;
                            sawRateControl = true;
                            break;
                        case "MaxBitrateMultiplier":
                            if (int.TryParse(value, out int mbm)) MaxBitrateMultiplier = mbm;
                            sawMaxBitrateMultiplier = true;
                            break;
                        case "GopSeconds":
                            if (int.TryParse(value, out int gop)) GopSeconds = gop;
                            sawGopSeconds = true;
                            break;
                        case "BFrames":
                            if (int.TryParse(value, out int bFrames)) BFrames = bFrames;
                            sawBFrames = true;
                            break;
                        case "LowLatency":
                            LowLatency = ParseBool(value);
                            sawLowLatency = true;
                            break;
                        case "QualityVsSpeed":
                            if (int.TryParse(value, out int qvs)) QualityVsSpeed = qvs;
                            sawQualityVsSpeed = true;
                            break;
                        case "EncoderIndex":
                            if (int.TryParse(value, out int encoderIndex)) EncoderIndex = encoderIndex;
                            sawEncoderIndex = true;
                            break;
                        case "CloudUrl":
                            CloudUrl = value;
                            sawCloudUrl = true;
                            break;
                        case "CloudApiKey":
                            CloudApiKey = value;
                            sawCloudApiKey = true;
                            break;
                        case "TargetScreenName":
                            TargetScreenName = value;
                            sawTargetScreenName = true;
                            break;
                        case "IsAiAutoSortEnabled":
                            IsAiAutoSortEnabled = ParseBool(value);
                            sawIsAiAutoSortEnabled = true;
                            break;
                    }
                }

                StoragePath = NormalizeStoragePath(StoragePath);
                if (string.IsNullOrWhiteSpace(Hotkey)) Hotkey = "Alt+Shift+F9";
                NormalizeAdvancedEngineSettings();

                if (!sawHotkey || !sawMicrophoneDeviceId || !sawMicrophoneDeviceName ||
                    !sawVideoCodec || !sawEncoderProfile || !sawRateControl || !sawMaxBitrateMultiplier ||
                    !sawGopSeconds || !sawBFrames || !sawLowLatency || !sawQualityVsSpeed || !sawEncoderIndex ||
                    !sawCloudUrl || !sawCloudApiKey || !sawTargetScreenName || !sawIsAiAutoSortEnabled ||
                    !string.Equals(loadedStoragePath, StoragePath, StringComparison.OrdinalIgnoreCase))
                {
                    Save();
                }
            }
            catch (Exception)
            {
                StoragePath = NormalizeStoragePath(StoragePath);
                if (string.IsNullOrWhiteSpace(Hotkey)) Hotkey = "Alt+Shift+F9";
                NormalizeAdvancedEngineSettings();
            }
        }

        public void Save()
        {
            try
            {
                StoragePath = NormalizeStoragePath(StoragePath);
                if (string.IsNullOrWhiteSpace(Hotkey)) Hotkey = "Alt+Shift+F9";
                NormalizeAdvancedEngineSettings();

                var lines = new[]
                {
                    $"BufferDuration={BufferDuration}",
                    $"FPS={FPS}",
                    $"Bitrate={Bitrate}",
                    $"StoragePath={StoragePath}",
                    $"Hotkey={Hotkey}",
                    $"MicrophoneDeviceId={MicrophoneDeviceId}",
                    $"MicrophoneDeviceName={MicrophoneDeviceName}",
                    $"VideoCodec={VideoCodec}",
                    $"EncoderProfile={EncoderProfile}",
                    $"RateControl={RateControl}",
                    $"MaxBitrateMultiplier={MaxBitrateMultiplier}",
                    $"GopSeconds={GopSeconds}",
                    $"BFrames={BFrames}",
                    $"LowLatency={LowLatency.ToString().ToLowerInvariant()}",
                    $"QualityVsSpeed={QualityVsSpeed}",
                    $"EncoderIndex={EncoderIndex}",
                    $"CloudUrl={CloudUrl}",
                    $"CloudApiKey={CloudApiKey}",
                    $"TargetScreenName={TargetScreenName}",
                    $"IsAiAutoSortEnabled={IsAiAutoSortEnabled.ToString().ToLowerInvariant()}"
                };
                File.WriteAllLines(_filePath, lines);
            }
            catch (Exception)
            {
                // Settings must never prevent the UI from opening.
            }
        }

        private static bool ParseBool(string value)
        {
            return value.Equals("1", StringComparison.OrdinalIgnoreCase) ||
                   value.Equals("true", StringComparison.OrdinalIgnoreCase) ||
                   value.Equals("yes", StringComparison.OrdinalIgnoreCase) ||
                   value.Equals("on", StringComparison.OrdinalIgnoreCase);
        }

        private static string CompactToken(string value)
        {
            return new string(value
                .Where(c => c != '-' && c != '_' && c != '.' && !char.IsWhiteSpace(c))
                .Select(char.ToLowerInvariant)
                .ToArray());
        }

        private void NormalizeAdvancedEngineSettings()
        {
            string codec = CompactToken(VideoCodec);
            VideoCodec = codec is "hevc" or "h265" ? "HEVC" : "H264";
            EncoderProfile = CompactToken(EncoderProfile) == "main" ? "Main" : "High";
            string rateControl = CompactToken(RateControl);
            RateControl = rateControl == "cbr"
                ? "CBR"
                : rateControl == "lowdelayvbr"
                    ? "LowDelayVBR"
                    : "VBR";
            MaxBitrateMultiplier = Math.Clamp(MaxBitrateMultiplier, 100, 400);
            GopSeconds = Math.Clamp(GopSeconds, 1, 10);
            BFrames = Math.Clamp(BFrames, 0, 4);
            QualityVsSpeed = Math.Clamp(QualityVsSpeed, 0, 100);
            EncoderIndex = Math.Max(0, EncoderIndex);
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
