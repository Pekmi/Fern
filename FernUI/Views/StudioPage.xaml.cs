using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
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
        private DispatcherTimer _singleTapTimer;
        private DispatcherTimer _controlsHideTimer;
        private bool _isSeeking = false;
        private bool _isStudioFullScreen;
        private bool _areControlsVisible = true;
        private DateTime _lastDoubleTapAt = DateTime.MinValue;
        private MediaPlaybackItem? _playbackItem;

        public StudioPage()
        {
            this.InitializeComponent();

            AudioTracks = new ObservableCollection<FernUI.Models.AudioTrack>();
            AudioTracksListView.ItemsSource = AudioTracks;

            _timer = new DispatcherTimer();
            _timer.Interval = TimeSpan.FromMilliseconds(200);
            _timer.Tick += Timer_Tick;

            _singleTapTimer = new DispatcherTimer();
            _singleTapTimer.Interval = TimeSpan.FromMilliseconds(240);
            _singleTapTimer.Tick += SingleTapTimer_Tick;

            _controlsHideTimer = new DispatcherTimer();
            _controlsHideTimer.Interval = TimeSpan.FromSeconds(2);
            _controlsHideTimer.Tick += ControlsHideTimer_Tick;
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
            _singleTapTimer.Stop();
            _controlsHideTimer.Stop();
            SetStudioFullScreen(false);

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
            TogglePlayback();
        }

        private void VideoSurface_Tapped(object sender, Microsoft.UI.Xaml.Input.TappedRoutedEventArgs e)
        {
            if ((DateTime.UtcNow - _lastDoubleTapAt).TotalMilliseconds < 300)
            {
                e.Handled = true;
                return;
            }

            _singleTapTimer.Stop();
            _singleTapTimer.Start();
            e.Handled = true;
        }

        private void VideoSurface_DoubleTapped(object sender, Microsoft.UI.Xaml.Input.DoubleTappedRoutedEventArgs e)
        {
            _lastDoubleTapAt = DateTime.UtcNow;
            _singleTapTimer.Stop();
            ToggleStudioFullScreen();
            e.Handled = true;
        }

        private void SingleTapTimer_Tick(object? sender, object e)
        {
            _singleTapTimer.Stop();
            TogglePlayback();
        }

        private void TogglePlayback()
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
            ToggleStudioFullScreen();
        }

        private void ToggleStudioFullScreen()
        {
            SetStudioFullScreen(!_isStudioFullScreen, true);
        }

        private void SetStudioFullScreen(bool isFullScreen, bool animate = false)
        {
            if (_isStudioFullScreen == isFullScreen) return;

            if (animate)
            {
                AnimateFullScreenTransition(() => ApplyStudioFullScreenState(isFullScreen));
                return;
            }

            ApplyStudioFullScreenState(isFullScreen);
        }

        private void ApplyStudioFullScreenState(bool isFullScreen)
        {
            _controlsHideTimer.Stop();

            _isStudioFullScreen = isFullScreen;

            if (App.MainWindow is MainWindow mainWindow)
            {
                mainWindow.SetContentFullScreen(isFullScreen);
            }

            StudioRoot.Padding = isFullScreen ? new Thickness(0) : new Thickness(32);
            StudioRoot.Background = isFullScreen
                ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0, 0, 0))
                : null;

            HeaderPanel.Visibility = isFullScreen ? Visibility.Collapsed : Visibility.Visible;
            InfoPanel.Visibility = isFullScreen ? Visibility.Collapsed : Visibility.Visible;

            Grid.SetRow(WorkspaceGrid, isFullScreen ? 0 : 1);
            Grid.SetRowSpan(WorkspaceGrid, isFullScreen ? 2 : 1);

            PlayerPanel.Margin = isFullScreen ? new Thickness(0) : new Thickness(0, 0, 24, 0);
            PlayerColumn.MinWidth = isFullScreen ? 0 : 500;

            if (isFullScreen)
            {
                InfoColumn.MinWidth = 0;
                InfoColumn.MaxWidth = 0;
                InfoColumn.Width = new GridLength(0);
                PlayerViewbox.HorizontalAlignment = HorizontalAlignment.Stretch;
                PlayerViewbox.VerticalAlignment = VerticalAlignment.Stretch;
                Grid.SetRow(ControlsPanel, 0);
                ControlsPanel.VerticalAlignment = VerticalAlignment.Bottom;
                ControlsPanel.Margin = new Thickness(32, 0, 32, 32);
                ControlsPanel.CornerRadius = new CornerRadius(12);
                ControlsPanel.BorderThickness = new Thickness(1);
                FullscreenIcon.Glyph = "\uE73F";
                ShowControls(true);
                RestartControlsHideTimer();
            }
            else
            {
                InfoColumn.MinWidth = 200;
                InfoColumn.MaxWidth = 400;
                InfoColumn.Width = new GridLength(300);
                PlayerViewbox.HorizontalAlignment = HorizontalAlignment.Center;
                PlayerViewbox.VerticalAlignment = VerticalAlignment.Center;
                Grid.SetRow(ControlsPanel, 1);
                ControlsPanel.VerticalAlignment = VerticalAlignment.Stretch;
                ControlsPanel.Margin = new Thickness(0);
                ControlsPanel.CornerRadius = new CornerRadius(0);
                ControlsPanel.BorderThickness = new Thickness(1);
                ShowControls(true);
                FullscreenIcon.Glyph = "\uE740";
            }
        }

        private void StudioRoot_PointerMoved(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
        {
            if (!_isStudioFullScreen) return;

            ShowControls();
            RestartControlsHideTimer();
        }

        private void ControlsHideTimer_Tick(object? sender, object e)
        {
            _controlsHideTimer.Stop();
            if (_isStudioFullScreen)
            {
                HideControls();
            }
        }

        private void RestartControlsHideTimer()
        {
            if (!_isStudioFullScreen) return;

            _controlsHideTimer.Stop();
            _controlsHideTimer.Start();
        }

        private void ShowControls(bool immediate = false)
        {
            if (_areControlsVisible && ControlsPanel.Opacity >= 1) return;

            _areControlsVisible = true;
            ControlsPanel.IsHitTestVisible = true;

            if (immediate)
            {
                ControlsPanel.Opacity = 1;
                return;
            }

            AnimateOpacity(ControlsPanel, 1, 140);
        }

        private void HideControls()
        {
            if (!_areControlsVisible) return;

            _areControlsVisible = false;
            var storyboard = CreateOpacityStoryboard(ControlsPanel, 0, 180);
            storyboard.Completed += (_, _) =>
            {
                if (!_areControlsVisible)
                {
                    ControlsPanel.IsHitTestVisible = false;
                }
            };
            storyboard.Begin();
        }

        private void AnimateFullScreenTransition(Action applyLayout)
        {
            var transform = PlayerViewbox.RenderTransform as CompositeTransform;
            if (transform == null)
            {
                applyLayout();
                return;
            }

            var shrink = new Storyboard();
            shrink.Children.Add(CreateDoubleAnimation(PlayerViewbox, "Opacity", 0.72, 110));
            shrink.Children.Add(CreateDoubleAnimation(PlayerViewbox, "(UIElement.RenderTransform).(CompositeTransform.ScaleX)", 0.985, 110));
            shrink.Children.Add(CreateDoubleAnimation(PlayerViewbox, "(UIElement.RenderTransform).(CompositeTransform.ScaleY)", 0.985, 110));
            shrink.Completed += (_, _) =>
            {
                applyLayout();

                var expand = new Storyboard();
                expand.Children.Add(CreateDoubleAnimation(PlayerViewbox, "Opacity", 1, 180));
                expand.Children.Add(CreateDoubleAnimation(PlayerViewbox, "(UIElement.RenderTransform).(CompositeTransform.ScaleX)", 1, 180));
                expand.Children.Add(CreateDoubleAnimation(PlayerViewbox, "(UIElement.RenderTransform).(CompositeTransform.ScaleY)", 1, 180));
                expand.Begin();
            };
            shrink.Begin();
        }

        private void AnimateOpacity(UIElement target, double to, double durationMs)
        {
            CreateOpacityStoryboard(target, to, durationMs).Begin();
        }

        private static Storyboard CreateOpacityStoryboard(UIElement target, double to, double durationMs)
        {
            var storyboard = new Storyboard();
            storyboard.Children.Add(CreateDoubleAnimation(target, "Opacity", to, durationMs));
            return storyboard;
        }

        private static DoubleAnimation CreateDoubleAnimation(UIElement target, string propertyPath, double to, double durationMs)
        {
            var animation = new DoubleAnimation
            {
                To = to,
                Duration = TimeSpan.FromMilliseconds(durationMs),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
            };

            Storyboard.SetTarget(animation, target);
            Storyboard.SetTargetProperty(animation, propertyPath);
            return animation;
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
