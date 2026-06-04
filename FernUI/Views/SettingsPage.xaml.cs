using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using System;
using FernUI.Models;

namespace FernUI.Views
{
    public sealed partial class SettingsPage : Page
    {
        private DispatcherTimer _debounceTimer;
        private bool _isLoading = true;

        public SettingsPage()
        {
            this.InitializeComponent();
            LoadSettings();
            SetupAutoSave();
            _isLoading = false;
        }

        private void LoadSettings()
        {
            var s = SettingsService.Instance;
            BufferSlider.Value = s.BufferDuration;
            BitrateSlider.Value = s.Bitrate;
            PathTextBox.Text = s.StoragePath;
            FpsSlider.Value = s.FPS;
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
            var s = SettingsService.Instance;
            s.BufferDuration = (int)BufferSlider.Value;
            s.Bitrate = (int)BitrateSlider.Value;
            s.StoragePath = PathTextBox.Text;
            s.FPS = (int)FpsSlider.Value;
            s.Save();
        }

        private void BrowseButton_Click(object sender, RoutedEventArgs e)
        {
            // Placeholder pour le sélecteur de dossier (FolderPicker nécéssite un HWND en WinUI 3)
            // Pour l'instant, on simule un changement
            PathTextBox.Text = @"C:\Videos\Fern";
            TriggerSave();
        }
    }

    public class StringFormatConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, string language)
        {
            if (parameter is string format)
            {
                return string.Format(format, value);
            }
            return value.ToString();
        }

        public object ConvertBack(object value, Type targetType, object parameter, string language)
        {
            throw new NotImplementedException();
        }
    }
}
