using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using System.Collections.ObjectModel;
using FernUI.Models;

namespace FernUI.Views
{
    public sealed partial class StudioPage : Page
    {
        public ObservableCollection<AudioTrack> AudioTracks { get; set; }
        private ClipModel? _selectedClip;

        public StudioPage()
        {
            this.InitializeComponent();

            AudioTracks = new ObservableCollection<AudioTrack>
            {
                new AudioTrack { Name = "Système", Icon = "\uE7F5", Volume = 100, PeakLevel = 60 },
                new AudioTrack { Name = "Microphone", Icon = "\uE720", Volume = 80, PeakLevel = 30 },
                new AudioTrack { Name = "Discord", Icon = "\uE191", Volume = 70, PeakLevel = 10 }
            };

            AudioTracksListView.ItemsSource = AudioTracks;
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);

            if (e.Parameter is ClipModel clip)
            {
                _selectedClip = clip;
                ClipTitleTextBlock.Text = _selectedClip.Title;
                GameBadge.Text = _selectedClip.Game.ToUpper();
                DurationText.Text = $"{_selectedClip.Date} - {_selectedClip.Duration}";
            }
        }

        private void BackButton_Click(object sender, RoutedEventArgs e)
        {
            if (Frame.CanGoBack)
            {
                Frame.GoBack();
            }
        }
    }
}
