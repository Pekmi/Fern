using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace FernUI.Models
{
    public class ClipModel : INotifyPropertyChanged
    {
        private string _title = string.Empty;
        private string _game = string.Empty;
        private string _date = string.Empty;
        private DateTime _creationDate = DateTime.Now;
        private string _duration = string.Empty;
        private string _resolution = string.Empty;
        private string _filePath = string.Empty;
        private string _thumbnailIcon = "\uE768";
        private ImageSource? _previewImage;
        private double _cardHeight = 200;
        private string _videoUrl = string.Empty;

        public event PropertyChangedEventHandler? PropertyChanged;

        public string Title { get => _title; set => SetProperty(ref _title, value); }
        public string Game { get => _game; set => SetProperty(ref _game, value); }
        public string Date { get => _date; set => SetProperty(ref _date, value); }
        public DateTime CreationDate { get => _creationDate; set => SetProperty(ref _creationDate, value); }
        public string FilePath { get => _filePath; set => SetProperty(ref _filePath, value); }
        public string ThumbnailIcon { get => _thumbnailIcon; set => SetProperty(ref _thumbnailIcon, value); }
        public ImageSource? PreviewImage { get => _previewImage; set => SetProperty(ref _previewImage, value); }
        public double CardHeight { get => _cardHeight; set => SetProperty(ref _cardHeight, value); }
        public string VideoUrl { get => _videoUrl; set => SetProperty(ref _videoUrl, value); }

        public string Duration
        {
            get => _duration;
            set
            {
                if (SetProperty(ref _duration, value))
                {
                    OnPropertyChanged(nameof(DurationVisibility));
                }
            }
        }

        public string Resolution
        {
            get => _resolution;
            set
            {
                if (SetProperty(ref _resolution, value))
                {
                    OnPropertyChanged(nameof(ResolutionVisibility));
                }
            }
        }

        public TimeSpan DurationValue { get; set; } = TimeSpan.Zero;
        public string CacheSignature { get; set; } = string.Empty;

        public Visibility DurationVisibility => string.IsNullOrEmpty(Duration) ? Visibility.Collapsed : Visibility.Visible;
        public Visibility ResolutionVisibility => string.IsNullOrEmpty(Resolution) ? Visibility.Collapsed : Visibility.Visible;

        private bool SetProperty<T>(ref T storage, T value, [CallerMemberName] string? propertyName = null)
        {
            if (EqualityComparer<T>.Default.Equals(storage, value)) return false;

            storage = value;
            OnPropertyChanged(propertyName);
            return true;
        }

        private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }

    public class AudioTrack
    {
        public string Name { get; set; } = string.Empty;
        public string Icon { get; set; } = string.Empty;
        public double Volume { get; set; }
        public double PeakLevel { get; set; }
    }

    public class CloudShareItem
    {
        public string Title { get; set; } = string.Empty;
        public string Game { get; set; } = string.Empty;
        public string Link { get; set; } = string.Empty;
        public string Expiration { get; set; } = string.Empty;
    }
}
