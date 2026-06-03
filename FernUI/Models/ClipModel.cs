using System;
using Microsoft.UI.Xaml;

namespace FernUI.Models
{
    public class ClipModel
    {
        public string Title { get; set; } = string.Empty;
        public string Game { get; set; } = string.Empty;
        public string Date { get; set; } = string.Empty;
        public DateTime CreationDate { get; set; } = DateTime.Now;
        public string Duration { get; set; } = string.Empty;
        public string ThumbnailIcon { get; set; } = "\uE768";
        public double CardHeight { get; set; } = 200;
        public string VideoUrl { get; set; } = string.Empty;

        public Visibility DurationVisibility => string.IsNullOrEmpty(Duration) ? Visibility.Collapsed : Visibility.Visible;
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
