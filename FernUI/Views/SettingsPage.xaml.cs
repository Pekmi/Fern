using FernUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Windows.Storage.Pickers;
using Windows.System;
using WinRT.Interop;

namespace FernUI.Views
{
    public sealed partial class SettingsPage : Page
    {
        private DispatcherTimer _debounceTimer = null!;
        private bool _isLoading = true;

        public SettingsPage()
        {
            InitializeComponent();
            LoadSettings();
            SetupAutoSave();
            _isLoading = false;
        }

        private void LoadSettings()
        {
            var settings = SettingsService.Instance;
            BufferSlider.Value = settings.BufferDuration;
            BitrateSlider.Value = settings.Bitrate;
            FpsSlider.Value = settings.FPS;
            PathTextBox.Text = SettingsService.NormalizeStoragePath(settings.StoragePath);
            HotkeyTextBox.Text = settings.Hotkey;
        }

        private void SetupAutoSave()
        {
            _debounceTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
            _debounceTimer.Tick += (s, e) =>
            {
                _debounceTimer.Stop();
                SaveSettings();
            };

            BufferSlider.ValueChanged += (s, e) => TriggerSave();
            BitrateSlider.ValueChanged += (s, e) => TriggerSave();
            FpsSlider.ValueChanged += (s, e) => TriggerSave();
        }

        private void TriggerSave()
        {
            if (_isLoading) return;

            _debounceTimer.Stop();
            _debounceTimer.Start();
        }

        private void SaveSettings()
        {
            var settings = SettingsService.Instance;
            settings.BufferDuration = (int)BufferSlider.Value;
            settings.Bitrate = (int)BitrateSlider.Value;
            settings.FPS = (int)FpsSlider.Value;
            settings.StoragePath = SettingsService.NormalizeStoragePath(PathTextBox.Text);
            settings.Hotkey = string.IsNullOrWhiteSpace(HotkeyTextBox.Text) ? "Alt+Shift+F9" : HotkeyTextBox.Text;
            settings.Save();

            PathTextBox.Text = settings.StoragePath;
            HotkeyTextBox.Text = settings.Hotkey;
        }

        private async void BrowseButton_Click(object sender, RoutedEventArgs e)
        {
            string oldPath = SettingsService.NormalizeStoragePath(PathTextBox.Text);
            string? selectedPath = await PickFolderAsync();
            if (selectedPath == null) return;

            string newPath = SettingsService.NormalizeStoragePath(selectedPath);
            if (string.Equals(oldPath, newPath, StringComparison.OrdinalIgnoreCase)) return;

            var dialog = new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = "Changer le dossier de stockage",
                Content = "Voulez-vous aussi deplacer le contenu de l'ancien dossier Fern vers la nouvelle destination ?",
                PrimaryButtonText = "Deplacer",
                SecondaryButtonText = "Garder",
                CloseButtonText = "Annuler",
                DefaultButton = ContentDialogButton.Primary
            };

            ContentDialogResult result = await dialog.ShowAsync();
            if (result == ContentDialogResult.None) return;

            PathTextBox.Text = newPath;
            SaveSettings();

            if (result == ContentDialogResult.Primary)
            {
                try
                {
                    await SettingsService.MoveStorageContentsAsync(oldPath, newPath);
                }
                catch (Exception ex)
                {
                    await ShowErrorAsync("Deplacement impossible", ex.Message);
                }
            }
        }

        private async Task<string?> PickFolderAsync()
        {
            var picker = new FolderPicker();
            picker.FileTypeFilter.Add("*");

            if (App.MainWindow != null)
            {
                IntPtr hwnd = WindowNative.GetWindowHandle(App.MainWindow);
                InitializeWithWindow.Initialize(picker, hwnd);
            }

            var folder = await picker.PickSingleFolderAsync();
            return folder?.Path;
        }

        private async Task ShowErrorAsync(string title, string message)
        {
            var dialog = new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = title,
                Content = message,
                CloseButtonText = "OK"
            };
            await dialog.ShowAsync();
        }

        private void HotkeyTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (IsModifierKey(e.Key))
            {
                e.Handled = true;
                return;
            }

            string keyName = FormatKey(e.Key);
            if (string.IsNullOrWhiteSpace(keyName))
            {
                e.Handled = true;
                return;
            }

            string hotkey = BuildHotkeyText(keyName);
            if (!string.IsNullOrWhiteSpace(hotkey))
            {
                HotkeyTextBox.Text = hotkey;
                SaveSettings();
            }

            e.Handled = true;
        }

        private static string BuildHotkeyText(string keyName)
        {
            string result = "";
            if (IsKeyDown(VirtualKey.Control)) result += "Ctrl+";
            if (IsKeyDown(VirtualKey.Menu)) result += "Alt+";
            if (IsKeyDown(VirtualKey.Shift)) result += "Shift+";
            if (IsKeyDown(VirtualKey.LeftWindows) || IsKeyDown(VirtualKey.RightWindows)) result += "Win+";
            return result + keyName;
        }

        private static string FormatKey(VirtualKey key)
        {
            if (key >= VirtualKey.A && key <= VirtualKey.Z) return key.ToString();
            if (key >= VirtualKey.Number0 && key <= VirtualKey.Number9) return ((int)key - (int)VirtualKey.Number0).ToString();
            if (key >= VirtualKey.F1 && key <= VirtualKey.F24) return key.ToString();

            return key switch
            {
                VirtualKey.Space => "Space",
                VirtualKey.Tab => "Tab",
                VirtualKey.Enter => "Enter",
                VirtualKey.Escape => "Esc",
                VirtualKey.Insert => "Insert",
                VirtualKey.Delete => "Delete",
                VirtualKey.Home => "Home",
                VirtualKey.End => "End",
                VirtualKey.PageUp => "PageUp",
                VirtualKey.PageDown => "PageDown",
                VirtualKey.Up => "Up",
                VirtualKey.Down => "Down",
                VirtualKey.Left => "Left",
                VirtualKey.Right => "Right",
                _ => ""
            };
        }

        private static bool IsModifierKey(VirtualKey key)
        {
            return key is VirtualKey.Control
                or VirtualKey.LeftControl
                or VirtualKey.RightControl
                or VirtualKey.Menu
                or VirtualKey.LeftMenu
                or VirtualKey.RightMenu
                or VirtualKey.Shift
                or VirtualKey.LeftShift
                or VirtualKey.RightShift
                or VirtualKey.LeftWindows
                or VirtualKey.RightWindows;
        }

        private static bool IsKeyDown(VirtualKey key)
        {
            return (GetKeyState((int)key) & 0x8000) != 0;
        }

        [DllImport("user32.dll")]
        private static extern short GetKeyState(int virtualKey);
    }

    public class StringFormatConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, string language)
        {
            if (parameter is string format)
            {
                return string.Format(format, value);
            }
            return value?.ToString() ?? "";
        }

        public object ConvertBack(object value, Type targetType, object parameter, string language)
        {
            throw new NotImplementedException();
        }
    }
}
