using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using System.Collections.ObjectModel;
using FernUI.Models;
using Windows.Media.Core;
using Windows.Media.Playback;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;

namespace FernUI.Views
{
    public sealed partial class StudioPage : Page
    {
        public ObservableCollection<FernUI.Models.AudioTrack> AudioTracks { get; set; }
        private ClipModel? _selectedClip;
        private DispatcherTimer _timer;
        private bool _isSeeking = false;
        private MediaPlaybackItem? _playbackItem;

        public StudioPage()
        {
            this.InitializeComponent();

            AudioTracks = new ObservableCollection<FernUI.Models.AudioTrack>();
            AudioTracksListView.ItemsSource = AudioTracks;

            _timer = new DispatcherTimer();
            _timer.Interval = TimeSpan.FromMilliseconds(200);
            _timer.Tick += Timer_Tick;
        }

        private void Timer_Tick(object? sender, object e)
        {
            if (!_isSeeking && StudioPlayer.MediaPlayer != null && StudioPlayer.MediaPlayer.PlaybackSession.NaturalDuration.TotalSeconds > 0)
            {
                ProgressSlider.Value = (StudioPlayer.MediaPlayer.PlaybackSession.Position.TotalSeconds / StudioPlayer.MediaPlayer.PlaybackSession.NaturalDuration.TotalSeconds) * 100;
            }
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

                if (!string.IsNullOrEmpty(_selectedClip.FilePath))
                {
                    InitializeMedia(_selectedClip.FilePath);
                }
            }
        }

        private void InitializeMedia(string filePath)
        {
            try
            {
                var source = MediaSource.CreateFromUri(new Uri(filePath));
                _playbackItem = new MediaPlaybackItem(source);
                
                // Détecter les pistes dès qu'elles changent ou sont prêtes
                _playbackItem.AudioTracksChanged += (s, args) => 
                {
                    DispatcherQueue.TryEnqueue(() => DetectAudioTracks(s));
                };

                StudioPlayer.Source = _playbackItem;
                StudioPlayer.MediaPlayer.PlaybackSession.PlaybackStateChanged += PlaybackSession_PlaybackStateChanged;
                _timer.Start();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Erreur chargement vidéo : {ex.Message}");
            }
        }

        private void DetectAudioTracks(MediaPlaybackItem item)
        {
            // Désabonnement global pour éviter les fuites
            foreach (var t in AudioTracks) t.OnVolumeChanged -= Track_OnVolumeChanged;
            
            AudioTracks.Clear();
            for (int i = 0; i < item.AudioTracks.Count; i++)
            {
                var track = item.AudioTracks[i];
                string name = !string.IsNullOrEmpty(track.Label) ? track.Label : $"Piste Audio {i + 1}";
                
                // Iconographie intelligente
                string icon = "\uE7F5";
                string lowerName = name.ToLower();
                if (lowerName.Contains("mic")) icon = "\uE720";
                else if (lowerName.Contains("game") || lowerName.Contains("jeu") || lowerName.Contains("system")) icon = "\uE7FC";
                else if (lowerName.Contains("discord") || lowerName.Contains("chat")) icon = "\uE191";

                var model = new FernUI.Models.AudioTrack 
                { 
                    Name = name, 
                    Icon = icon, 
                    Volume = 100, 
                    PeakLevel = 0 
                };
                
                model.OnVolumeChanged += Track_OnVolumeChanged;
                AudioTracks.Add(model);
            }
            
            // Note technique : Le MediaPlayer standard WinUI 3 ne joue qu'une piste à la fois.
            // Le mixage multi-pistes simultané nécessite un moteur AudioGraph synchronisé
            // qui sera implémenté si le besoin de "Mixing live" est confirmé.
        }

        private void Track_OnVolumeChanged(FernUI.Models.AudioTrack track, double newVolume)
        {
            // Logique de mixage à venir
            System.Diagnostics.Debug.WriteLine($"Studio: Volume {track.Name} -> {newVolume}%");
        }

        private void PlaybackSession_PlaybackStateChanged(MediaPlaybackSession sender, object args)
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                if (sender.PlaybackState == MediaPlaybackState.Playing)
                {
                    PlayPauseIcon.Glyph = "\uE769";
                }
                else
                {
                    PlayPauseIcon.Glyph = "\uE768";
                }
            });
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            base.OnNavigatedFrom(e);
            _timer.Stop();

            foreach (var t in AudioTracks) t.OnVolumeChanged -= Track_OnVolumeChanged;

            if (StudioPlayer.MediaPlayer != null)
            {
                StudioPlayer.MediaPlayer.PlaybackSession.PlaybackStateChanged -= PlaybackSession_PlaybackStateChanged;
                StudioPlayer.MediaPlayer.Pause();
                StudioPlayer.Source = null;
            }
            _playbackItem = null;
        }

        private void PlayPauseButton_Click(object sender, RoutedEventArgs e)
        {
            if (StudioPlayer.MediaPlayer == null) return;

            if (StudioPlayer.MediaPlayer.PlaybackSession.PlaybackState == MediaPlaybackState.Playing)
            {
                StudioPlayer.MediaPlayer.Pause();
            }
            else
            {
                StudioPlayer.MediaPlayer.Play();
            }
        }

        private void ProgressSlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            if (StudioPlayer.MediaPlayer != null && StudioPlayer.MediaPlayer.PlaybackSession.NaturalDuration.TotalSeconds > 0)
            {
                var position = (e.NewValue / 100.0) * StudioPlayer.MediaPlayer.PlaybackSession.NaturalDuration.TotalSeconds;
                var timeSpan = TimeSpan.FromSeconds(position);
                
                if (Math.Abs((timeSpan - StudioPlayer.MediaPlayer.PlaybackSession.Position).TotalSeconds) > 0.5)
                {
                    StudioPlayer.MediaPlayer.PlaybackSession.Position = timeSpan;
                }
            }
        }

        private void MuteButton_Click(object sender, RoutedEventArgs e)
        {
            if (StudioPlayer.MediaPlayer == null) return;
            StudioPlayer.MediaPlayer.IsMuted = !StudioPlayer.MediaPlayer.IsMuted;
            VolumeIcon.Glyph = StudioPlayer.MediaPlayer.IsMuted ? "\uE74F" : "\uE767";
        }

        private void VolumeSlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            if (StudioPlayer.MediaPlayer == null) return;
            StudioPlayer.MediaPlayer.Volume = e.NewValue / 100.0;
        }

        private void FullscreenButton_Click(object sender, RoutedEventArgs e)
        {
            StudioPlayer.IsFullWindow = !StudioPlayer.IsFullWindow;
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