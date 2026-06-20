using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Navigation;
using System.Collections.ObjectModel;
using FernUI.Models;
using Windows.ApplicationModel.DataTransfer;
using Windows.Media.Core;
using Windows.Media.Playback;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using Windows.Foundation;
using Windows.Foundation.Collections;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace FernUI.Views
{
    public sealed partial class StudioPage : Page
    {
        public ObservableCollection<FernUI.Models.AudioTrack> AudioTracks { get; } = new();
        private ClipModel? _selectedClip;
        private DispatcherTimer _timer;
        private DispatcherTimer _singleTapTimer;
        private DispatcherTimer _controlsHideTimer;
        private DispatcherTimer _saveFeedbackTimer;
        private bool _isSeeking = false;
        private bool _isUpdatingSlider = false;
        private bool _isVideoSeeking = false;
        private TimeSpan? _pendingSeekPosition;
        private bool _isStudioFullScreen;
        private bool _areControlsVisible = true;
        private DateTime _lastDoubleTapAt = DateTime.MinValue;
        private MediaPlaybackItem? _playbackItem;
        private MediaPlayer? _videoPlayer;
        private Dictionary<int, AudioTrackManifestInfo> _audioTrackManifest = new();
        private readonly Dictionary<int, MediaPlayer> _audioPlayers = new();
        private readonly Dictionary<int, int> _audioPlayerTrackSelections = new();
        private double _masterDb = 0.0;
        private double _studioVolume = 1.0;
        private bool _isUpdatingStudioVolumeControls;
        private bool _isUpdatingMasterVolumeControls;
        private bool _isNormalizingLufs;
        private bool IsNormalizingLufs
        {
            get => _isNormalizingLufs;
            set
            {
                _isNormalizingLufs = value;
                AudioProcessingProgress.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
            }
        }
        private bool _isMuted;
        private bool _isTearingDownStudioSession;
        private TypedEventHandler<MediaPlaybackItem, IVectorChangedEventArgs>? _playbackItemAudioTracksChangedHandler;
        private ContentDialog? _exportDialog;
        private Slider? _exportSizeSlider;
        private TextBlock? _exportSizeText;
        private ProgressBar? _exportProgressBar;
        private ProgressBar? _uploadProgressBar;
        private TextBlock? _exportStatusText;
        private TextBlock? _uploadStatusText;
        private StorageFile? _lastExportedFile;
        private CancellationTokenSource? _lufsAnalysisCancellation;
        private int _lufsAnalysisGeneration;
        private StudioLufsAnalysis? _lastLufsAnalysis;

        private sealed class AudioTrackManifestInfo
        {
            public string Name { get; set; } = string.Empty;
            public string SidecarPath { get; set; } = string.Empty;
            public int SidecarAudioIndex { get; set; }
            public double ActiveRatio { get; set; }
            public double? Volume { get; set; }
            public double LufsGain { get; set; } = 1.0;
        }

        public StudioPage()
        {
            this.InitializeComponent();

            SetMasterDb(0, false);
            AudioTracksListView.ItemsSource = AudioTracks;
            EnsureVideoPlayer();

            _timer = new DispatcherTimer();
            _timer.Interval = TimeSpan.FromMilliseconds(200);
            _timer.Tick += Timer_Tick;

            _singleTapTimer = new DispatcherTimer();
            _singleTapTimer.Interval = TimeSpan.FromMilliseconds(240);
            _singleTapTimer.Tick += SingleTapTimer_Tick;

            _controlsHideTimer = new DispatcherTimer();
            _controlsHideTimer.Interval = TimeSpan.FromSeconds(2);
            _controlsHideTimer.Tick += ControlsHideTimer_Tick;

            _saveFeedbackTimer = new DispatcherTimer();
            _saveFeedbackTimer.Interval = TimeSpan.FromSeconds(1.4);
            _saveFeedbackTimer.Tick += SaveFeedbackTimer_Tick;

            ProgressSlider.AddHandler(UIElement.PointerPressedEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler(Slider_PointerPressed), true);
            ProgressSlider.AddHandler(UIElement.PointerReleasedEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler(Slider_PointerReleased), true);
            ProgressSlider.AddHandler(UIElement.PointerCanceledEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler(Slider_PointerReleased), true);
            ProgressSlider.AddHandler(UIElement.PointerCaptureLostEvent, new Microsoft.UI.Xaml.Input.PointerEventHandler(Slider_PointerReleased), true);
        }

        private void Slider_PointerPressed(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
        {
            _isSeeking = true;
        }

        private void Slider_PointerReleased(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
        {
            _isSeeking = false;
            if (_videoPlayer != null && _videoPlayer.PlaybackSession.NaturalDuration.TotalSeconds > 0)
            {
                RequestSeek(TimeSpan.FromSeconds(ProgressSlider.Value));
            }
        }

        private void RequestSeek(TimeSpan targetPosition)
        {
            if (_videoPlayer == null) return;

            var session = _videoPlayer.PlaybackSession;
            if (session.NaturalDuration <= TimeSpan.Zero) return;

            TimeSpan clamped = targetPosition;
            if (clamped < TimeSpan.Zero) clamped = TimeSpan.Zero;
            if (clamped > session.NaturalDuration) clamped = session.NaturalDuration;

            if (Math.Abs((clamped - session.Position).TotalMilliseconds) < 120)
            {
                _isVideoSeeking = false;
                _pendingSeekPosition = null;
                return;
            }

            if (_isVideoSeeking && _pendingSeekPosition.HasValue &&
                Math.Abs((_pendingSeekPosition.Value - clamped).TotalMilliseconds) < 120)
            {
                return;
            }

            _pendingSeekPosition = clamped;
            _isVideoSeeking = true;
            PauseAudioPlayers();
            session.Position = clamped;
        }

        private void EnsureVideoPlayer()
        {
            if (_videoPlayer != null) return;

            _videoPlayer = new MediaPlayer
            {
                AutoPlay = false,
                IsMuted = true,
                Volume = 0
            };
            StudioPlayer.SetMediaPlayer(_videoPlayer);
            _videoPlayer.PlaybackSession.PlaybackStateChanged += PlaybackSession_PlaybackStateChanged;
            _videoPlayer.PlaybackSession.SeekCompleted += PlaybackSession_SeekCompleted;
        }

        private void DisposeVideoPlayer()
        {
            if (_videoPlayer == null) return;

            try
            {
                _videoPlayer.PlaybackSession.PlaybackStateChanged -= PlaybackSession_PlaybackStateChanged;
                _videoPlayer.PlaybackSession.SeekCompleted -= PlaybackSession_SeekCompleted;
                _videoPlayer.Pause();
                StudioPlayer.SetMediaPlayer(null);
                _videoPlayer.Source = null;
                _videoPlayer.Dispose();
            }
            catch
            {
                // Ignore disposal failures from partially-closed media objects.
            }
            finally
            {
                _videoPlayer = null;
            }
        }

        private void Timer_Tick(object? sender, object e)
        {
            if (!_isSeeking && !_isVideoSeeking && _videoPlayer != null && _videoPlayer.PlaybackSession.NaturalDuration.TotalSeconds > 0)
            {
                _isUpdatingSlider = true;
                if (ProgressSlider.Maximum != _videoPlayer.PlaybackSession.NaturalDuration.TotalSeconds)
                {
                    ProgressSlider.Maximum = _videoPlayer.PlaybackSession.NaturalDuration.TotalSeconds;
                }
                ProgressSlider.Value = _videoPlayer.PlaybackSession.Position.TotalSeconds;
                _isUpdatingSlider = false;
                if (_videoPlayer.PlaybackSession.PlaybackState == MediaPlaybackState.Playing)
                {
                    SynchronizeAudioPlayersToVideo(false);
                }
            }
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            _isTearingDownStudioSession = false;
            EnsureVideoPlayer();

            if (e.Parameter is ClipModel clip)
            {
                _selectedClip = clip;
                ClipTitleTextBox.Text = _selectedClip.Title;
                DurationText.Text = string.Join("   ", new[] { _selectedClip.Date, _selectedClip.Duration }
                    .Where(value => !string.IsNullOrWhiteSpace(value)));
                SetStudioVolumePercent(100, false);
                SetMasterDb(0, false);

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
                DisposeAudioPlayers();
                DetachPlaybackItemHandlers();
                EnsureVideoPlayer();
                _audioTrackManifest = LoadAudioTrackManifest(filePath);
                var source = MediaSource.CreateFromUri(new Uri(filePath));
                _playbackItem = new MediaPlaybackItem(source);
                
                // Détecter les pistes dès qu'elles changent ou sont prêtes
                _playbackItemAudioTracksChangedHandler = PlaybackItem_AudioTracksChanged;
                _playbackItem.AudioTracksChanged += _playbackItemAudioTracksChangedHandler;

                if (_videoPlayer != null)
                {
                    _videoPlayer.IsMuted = true;
                    _videoPlayer.Volume = 0;
                    _videoPlayer.Source = _playbackItem;
                }
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
            var detectedTracks = new List<FernUI.Models.AudioTrack>();
            for (int i = 0; i < item.AudioTracks.Count; i++)
            {
                var track = item.AudioTracks[i];
                AudioTrackManifestInfo? manifestInfo = _audioTrackManifest.TryGetValue(i, out AudioTrackManifestInfo? info) ? info : null;
                string name = manifestInfo != null && !string.IsNullOrWhiteSpace(manifestInfo.Name)
                    ? manifestInfo.Name
                    : !string.IsNullOrEmpty(track.Label) ? track.Label : $"Piste Audio {i + 1}";
                
                // Iconographie intelligente
                string icon = "\uE7F5";
                string lowerName = name.ToLower();
                bool isVoiceMeeter = lowerName.Contains("voicemeeter");
                double defaultVolume = isVoiceMeeter ? 0 : 100;
                if (lowerName.Contains("mic")) icon = "\uE720";
                else if (lowerName.Contains("game") || lowerName.Contains("jeu") || lowerName.Contains("system")) icon = "\uE7FC";
                else if (lowerName.Contains("discord") || lowerName.Contains("chat")) icon = "\uE191";

                var model = new FernUI.Models.AudioTrack 
                { 
                    AudioIndex = i,
                    Name = name, 
                    Icon = icon, 
                    Volume = manifestInfo?.Volume ?? defaultVolume,
                    LufsGain = manifestInfo?.LufsGain ?? 1.0,
                    SidecarPath = manifestInfo?.SidecarPath ?? string.Empty,
                    SidecarAudioIndex = manifestInfo?.SidecarAudioIndex ?? 0,
                    ActiveRatio = manifestInfo?.ActiveRatio ?? 0,
                    PeakLevel = 0 
                };
                
                model.OnVolumeChanged += Track_OnVolumeChanged;
                detectedTracks.Add(model);
            }

            bool isEqualized = false;
            foreach (var track in detectedTracks)
            {
                if (Math.Abs(track.LufsGain - 1.0) > 0.001) isEqualized = true;
            }

            EqualizeTracksToggle.Toggled -= EqualizeTracksToggle_Toggled;
            EqualizeTracksToggle.IsOn = isEqualized;
            EqualizeTracksToggle.Toggled += EqualizeTracksToggle_Toggled;

            foreach (var model in detectedTracks
                .OrderBy(track => track.Name.Contains("voicemeeter", StringComparison.OrdinalIgnoreCase))
                .ThenByDescending(track => track.ActiveRatio)
                .ThenBy(track => track.AudioIndex))
            {
                AudioTracks.Add(model);
            }
            
            CreateAudioPlayers();
            ApplyAllAudioVolumes();
            StartLufsAnalysis();
            if (_videoPlayer?.PlaybackSession.PlaybackState == MediaPlaybackState.Playing)
            {
                SynchronizeAudioPlayersToVideo(true);
                PlayAudioPlayers();
            }
        }

        private void Track_OnVolumeChanged(FernUI.Models.AudioTrack track, double newVolume)
        {
            ApplyTrackVolume(track);
        }

        private void PlaybackSession_PlaybackStateChanged(MediaPlaybackSession sender, object args)
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_isTearingDownStudioSession || _videoPlayer?.PlaybackSession != sender)
                {
                    return;
                }

                if (sender.PlaybackState == MediaPlaybackState.Playing)
                {
                    PlayPauseIcon.Glyph = "\uE769";
                    SynchronizeAudioPlayersToVideo(true);
                    PlayAudioPlayers();
                }
                else
                {
                    PlayPauseIcon.Glyph = "\uE768";
                    PauseAudioPlayers();
                }
            });
        }

        private void PlaybackSession_SeekCompleted(MediaPlaybackSession sender, object args)
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_isTearingDownStudioSession || _videoPlayer?.PlaybackSession != sender)
                {
                    return;
                }

                _isVideoSeeking = false;
                _pendingSeekPosition = null;

                if (_videoPlayer != null)
                {
                    _isUpdatingSlider = true;
                    ProgressSlider.Value = _videoPlayer.PlaybackSession.Position.TotalSeconds;
                    _isUpdatingSlider = false;
                }

                SynchronizeAudioPlayersToVideo(true);

                if (sender.PlaybackState == MediaPlaybackState.Playing)
                {
                    PlayAudioPlayers();
                }
            });
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            base.OnNavigatedFrom(e);
            ReleaseStudioSessionResources();
        }

        private void ReleaseStudioSessionResources()
        {
            _isTearingDownStudioSession = true;
            _timer.Stop();
            _singleTapTimer.Stop();
            _controlsHideTimer.Stop();
            _saveFeedbackTimer.Stop();
            SetStudioFullScreen(false);
            _isSeeking = false;
            _isUpdatingSlider = false;
            _isVideoSeeking = false;
            _pendingSeekPosition = null;

            foreach (var t in AudioTracks) t.OnVolumeChanged -= Track_OnVolumeChanged;
            AudioTracks.Clear();
            _audioTrackManifest.Clear();
            DisposeAudioPlayers();
            DetachPlaybackItemHandlers();
            CancelLufsAnalysis();
            HideLufsWarning();

            DisposeVideoPlayer();
            _playbackItem = null;
            _selectedClip = null;

            _isUpdatingSlider = true;
            ProgressSlider.Value = 0;
            ProgressSlider.Maximum = 1;
            _isUpdatingSlider = false;
            PlayPauseIcon.Glyph = "\uE768";
            MemoryCleanup.CollectNow();
        }

        private void PlayPauseButton_Click(object sender, RoutedEventArgs e)
        {
            TogglePlayback();
        }

        private async void SaveButton_Click(object sender, RoutedEventArgs e)
        {
            bool saved = SaveCurrentAudioMix();
            ShowSaveFeedback(saved);
        }

        private async void ShareButton_Click(object sender, RoutedEventArgs e)
        {
            await ShareClipAsync();
        }

        private async Task ShareClipAsync()
        {
            if (_selectedClip == null || string.IsNullOrWhiteSpace(_selectedClip.FilePath) || !File.Exists(_selectedClip.FilePath))
            {
                return;
            }

            _exportDialog = new ContentDialog
            {
                XamlRoot = XamlRoot,
                CloseButtonText = string.Empty,
                DefaultButton = ContentDialogButton.None
            };

            ShowShareProgressContent();
            var dialogTask = _exportDialog.ShowAsync();

            try
            {
                long sourceBytes = new FileInfo(_selectedClip.FilePath).Length;
                double targetSizeMb = Math.Max(0.1, sourceBytes / 1024.0 / 1024.0);
                IReadOnlyList<FernUI.Models.AudioTrack> tracks = AudioTracks.ToList();
                TimeSpan duration = _selectedClip.DurationValue > TimeSpan.Zero
                    ? _selectedClip.DurationValue
                    : _videoPlayer?.PlaybackSession.NaturalDuration ?? TimeSpan.Zero;
                
                SaveCurrentAudioMix();

                var progress = new Progress<double>(value =>
                {
                    if (_exportProgressBar == null) return;
                    _exportProgressBar.IsIndeterminate = false;
                    _exportProgressBar.Value = Math.Clamp(value, 0, 100);
                    if (_exportStatusText != null)
                    {
                        _exportStatusText.Text = $"Export... {Math.Clamp(value, 0, 100):0}%";
                    }
                });

                StorageFile exportedFile = await StudioExportService.ExportAsync(_selectedClip.FilePath, tracks, targetSizeMb, duration, CurrentMasterGain, progress);
                
                if (_exportStatusText != null) _exportStatusText.Text = "Export terminé";
                if (_exportProgressBar != null) _exportProgressBar.Value = 100;
                if (_uploadStatusText != null) _uploadStatusText.Text = "Upload... 0%";
                if (_uploadProgressBar != null)
                {
                    _uploadProgressBar.IsIndeterminate = false;
                    _uploadProgressBar.Value = 0;
                }

                var uploadProgress = new Progress<double>(value =>
                {
                    double clamped = Math.Clamp(value, 0, 100);
                    if (_uploadProgressBar != null)
                    {
                        _uploadProgressBar.IsIndeterminate = false;
                        _uploadProgressBar.Value = clamped;
                    }
                    if (_uploadStatusText != null)
                    {
                        _uploadStatusText.Text = $"Upload... {clamped:0}%";
                    }
                });

                string link = await CloudService.UploadClipAsync(exportedFile.Path, _selectedClip.Title, uploadProgress);
                
                var package = new DataPackage();
                package.SetText(link);
                Clipboard.SetContent(package);

                ShowShareCompleteContent(link);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Cloud Upload failed: {ex}");
                ShowExportErrorContent(ex.Message);
            }
        }

        private void ShowShareProgressContent()
        {
            if (_exportDialog == null) return;

            _exportDialog.CloseButtonText = string.Empty;
            _exportDialog.Content = new StackPanel
            {
                Spacing = 16,
                Children =
                {
                    new TextBlock
                    {
                        Text = "Partage en cours",
                        FontWeight = Microsoft.UI.Text.FontWeights.SemiBold
                    },
                    (_exportStatusText = new TextBlock
                    {
                        Text = "Pr\u00e9paration...",
                        Opacity = 0.72
                    }),
                    (_exportProgressBar = new ProgressBar
                    {
                        Minimum = 0,
                        Maximum = 100,
                        IsIndeterminate = true
                    }),
                    (_uploadStatusText = new TextBlock
                    {
                        Text = "Upload en attente",
                        Opacity = 0.72
                    }),
                    (_uploadProgressBar = new ProgressBar
                    {
                        Minimum = 0,
                        Maximum = 100,
                        Value = 0,
                        IsIndeterminate = false
                    })
                }
            };
        }

        private void ShowShareCompleteContent(string link)
        {
            if (_exportDialog == null) return;

            _exportDialog.CloseButtonText = string.Empty;
            var root = new StackPanel { Spacing = 16 };
            root.Children.Add(new TextBlock
            {
                Text = "Partage r\u00e9ussi",
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold
            });

            var linkButton = new Button
            {
                Content = new TextBlock
                {
                    Text = link,
                    TextWrapping = TextWrapping.Wrap,
                    Foreground = (Brush)Application.Current.Resources["AccentTextFillColorPrimaryBrush"]
                },
                HorizontalAlignment = HorizontalAlignment.Stretch,
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(12, 255, 255, 255)),
                Padding = new Thickness(12)
            };
            ToolTipService.SetToolTip(linkButton, "Cliquer pour copier");

            linkButton.Click += (_, _) =>
            {
                var package = new DataPackage();
                package.SetText(link);
                Clipboard.SetContent(package);
                
                // Petit feedback visuel au clic
                linkButton.Content = new TextBlock { Text = "Lien copi\u00e9 !", Foreground = linkButton.Foreground };
                _ = Task.Delay(1500).ContinueWith(t => DispatcherQueue.TryEnqueue(() => linkButton.Content = new TextBlock { Text = link, Foreground = linkButton.Foreground, TextWrapping = TextWrapping.Wrap }));
            };

            root.Children.Add(linkButton);

            var doneButton = new Button
            {
                Content = "Termin\u00e9",
                Style = SaveButton.Style,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                Padding = new Thickness(0, 12, 0, 12)
            };
            doneButton.Click += (_, _) => _exportDialog?.Hide();
            root.Children.Add(doneButton);

            _exportDialog.Content = root;
        }

        private bool SaveCurrentAudioMix()
        {
            if (_selectedClip == null || string.IsNullOrWhiteSpace(_selectedClip.FilePath))
            {
                return false;
            }

            string packagePath = Path.ChangeExtension(_selectedClip.FilePath, ".fern");
            if (!File.Exists(packagePath))
            {
                System.Diagnostics.Debug.WriteLine($"Studio: paquet Fern introuvable pour sauvegarde: {packagePath}");
                return false;
            }

            try
            {
                SaveAudioVolumesToFernPackage(packagePath, AudioTracks);
                return true;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Studio: sauvegarde mix audio impossible: {ex.Message}");
                return false;
            }
        }

        private void ShowSaveFeedback(bool saved)
        {
            _saveFeedbackTimer.Stop();

            SaveButton.IsEnabled = false;
            SaveButtonIcon.Glyph = saved ? "\uE73E" : "\uE783";
            SaveButtonText.Text = saved ? "Sauvegard\u00e9" : "Erreur";

            if (SaveButton.RenderTransform is CompositeTransform transform)
            {
                transform.ScaleX = 0.97;
                transform.ScaleY = 0.97;

                var storyboard = new Storyboard();
                storyboard.Children.Add(CreateDoubleAnimation(SaveButton, "(UIElement.RenderTransform).(CompositeTransform.ScaleX)", 1, 180));
                storyboard.Children.Add(CreateDoubleAnimation(SaveButton, "(UIElement.RenderTransform).(CompositeTransform.ScaleY)", 1, 180));
                storyboard.Begin();
            }

            _saveFeedbackTimer.Start();
        }

        private void SaveFeedbackTimer_Tick(object? sender, object e)
        {
            _saveFeedbackTimer.Stop();
            SaveButton.IsEnabled = true;
            SaveButtonIcon.Glyph = "\uE74E";
            SaveButtonText.Text = "Sauvegarder";
        }

        private async void ExportButton_Click(object sender, RoutedEventArgs e)
        {
            await ShowExportDialogAsync();
        }

        private async Task ShowExportDialogAsync()
        {
            if (_selectedClip == null || string.IsNullOrWhiteSpace(_selectedClip.FilePath) || !File.Exists(_selectedClip.FilePath))
            {
                return;
            }

            long sourceBytes = new FileInfo(_selectedClip.FilePath).Length;
            double maximumMb = Math.Max(0.1, sourceBytes / 1024.0 / 1024.0);
            double minimumMb = Math.Min(5.0, maximumMb);
            _lastExportedFile = null;

            _exportDialog = new ContentDialog
            {
                XamlRoot = XamlRoot,
                CloseButtonText = string.Empty,
                DefaultButton = ContentDialogButton.None,
                Content = CreateExportSizePickerContent(minimumMb, maximumMb)
            };

            await _exportDialog.ShowAsync();
            _exportDialog = null;
            _exportSizeSlider = null;
            _exportSizeText = null;
            _exportProgressBar = null;
            _uploadProgressBar = null;
            _exportStatusText = null;
            _uploadStatusText = null;
        }

        private UIElement CreateExportSizePickerContent(double minimumMb, double maximumMb)
        {
            var root = CreateExportDialogGrid();

            var leftPanel = new StackPanel { Spacing = 14 };
            leftPanel.Children.Add(new TextBlock
            {
                Text = "Taille souhait\u00e9e",
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold
            });

            _exportSizeSlider = new Slider
            {
                Minimum = minimumMb,
                Maximum = maximumMb,
                Value = maximumMb,
                StepFrequency = maximumMb <= 10 ? 0.1 : 1,
                TickFrequency = maximumMb <= 10 ? 0.5 : 5
            };
            _exportSizeSlider.ValueChanged += (_, _) => UpdateExportSizeText();
            leftPanel.Children.Add(_exportSizeSlider);

            _exportSizeText = new TextBlock
            {
                Opacity = 0.72
            };
            leftPanel.Children.Add(_exportSizeText);
            UpdateExportSizeText();

            Grid.SetColumn(leftPanel, 0);
            root.Children.Add(leftPanel);

            var actions = new StackPanel { Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
            var exportButton = new Button
            {
                Content = "Exporter",
                HorizontalAlignment = HorizontalAlignment.Center,
                Width = 160,
                Padding = new Thickness(0, 10, 0, 10),
                MinHeight = 40
            };
            exportButton.Click += (_, _) => _ = ExportFromDialogAsync();
            actions.Children.Add(exportButton);

            var cancelButton = new Button
            {
                Content = "Annuler",
                HorizontalAlignment = HorizontalAlignment.Center,
                Width = 160,
                Padding = new Thickness(0, 10, 0, 10),
                MinHeight = 40
            };
            cancelButton.Click += (_, _) => _exportDialog?.Hide();
            actions.Children.Add(cancelButton);
            Grid.SetColumn(actions, 1);
            root.Children.Add(actions);

            return root;
        }

        private void UpdateExportSizeText()
        {
            if (_exportSizeSlider == null || _exportSizeText == null) return;

            _exportSizeText.Text =
                $"{FormatMegabytes(_exportSizeSlider.Value)} / {FormatMegabytes(_exportSizeSlider.Maximum)}";
        }

        private static Grid CreateExportDialogGrid()
        {
            var grid = new Grid
            {
                ColumnSpacing = 14,
                Width = 520,
                MaxWidth = 520
            };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(6, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(4, GridUnitType.Star) });
            return grid;
        }

        private async Task ExportFromDialogAsync()
        {
            if (_selectedClip == null || _exportDialog == null || _exportSizeSlider == null)
            {
                return;
            }

            string sourcePath = _selectedClip.FilePath;
            double targetSizeMb = _exportSizeSlider.Value;
            IReadOnlyList<FernUI.Models.AudioTrack> tracks = AudioTracks.ToList();
            TimeSpan duration = _selectedClip.DurationValue > TimeSpan.Zero
                ? _selectedClip.DurationValue
                : _videoPlayer?.PlaybackSession.NaturalDuration ?? TimeSpan.Zero;
            SaveCurrentAudioMix();

            ShowExportProgressContent();

            try
            {
                var progress = new Progress<double>(value =>
                {
                    if (_exportProgressBar == null) return;
                    _exportProgressBar.IsIndeterminate = false;
                    _exportProgressBar.Value = Math.Clamp(value, 0, 100);
                    if (_exportStatusText != null)
                    {
                        _exportStatusText.Text = $"Traitement... {Math.Clamp(value, 0, 100):0}%";
                    }
                });

                _lastExportedFile = await StudioExportService.ExportAsync(sourcePath, tracks, targetSizeMb, duration, CurrentMasterGain, progress);
                ShowExportCompleteContent(_lastExportedFile);
            }
            catch (Exception ex)
            {
                ShowExportErrorContent(ex.Message);
            }
        }

        private void ShowExportProgressContent()
        {
            if (_exportDialog == null) return;

            _exportDialog.CloseButtonText = string.Empty;
            _exportDialog.Content = new StackPanel
            {
                Spacing = 16,
                Children =
                {
                    new TextBlock
                    {
                        Text = "Export en cours",
                        FontWeight = Microsoft.UI.Text.FontWeights.SemiBold
                    },
                    (_exportStatusText = new TextBlock
                    {
                        Text = "Pr\u00e9paration...",
                        Opacity = 0.72
                    }),
                    (_exportProgressBar = new ProgressBar
                    {
                        Minimum = 0,
                        Maximum = 100,
                        IsIndeterminate = true
                    })
                }
            };
        }

        private void ShowExportCompleteContent(StorageFile exportedFile)
        {
            if (_exportDialog == null) return;

            _exportDialog.CloseButtonText = string.Empty;
            FileInfo fileInfo = new(exportedFile.Path);

            var root = CreateExportDialogGrid();

            var dragBlock = new Border
            {
                CanDrag = true,
                Padding = new Thickness(18),
                CornerRadius = new CornerRadius(8),
                BorderThickness = new Thickness(1),
                BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(96, 255, 255, 255)),
                Background = new SolidColorBrush(Windows.UI.Color.FromArgb(18, 255, 255, 255)),
                Child = new StackPanel
                {
                    Spacing = 8,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    Children =
                    {
                        new FontIcon { Glyph = "\uE714", FontSize = 28 },
                        new TextBlock
                        {
                            Text = exportedFile.Name,
                            TextAlignment = TextAlignment.Center,
                            TextWrapping = TextWrapping.Wrap,
                            MaxLines = 2,
                            TextTrimming = TextTrimming.CharacterEllipsis
                        },
                        new TextBlock
                        {
                            Text = FormatFileSize(fileInfo.Length),
                            TextAlignment = TextAlignment.Center,
                            Opacity = 0.72
                        }
                    }
                }
            };
            dragBlock.MinHeight = 112;
            dragBlock.DragStarting += (_, args) =>
            {
                if (_lastExportedFile == null)
                {
                    args.Cancel = true;
                    return;
                }

                args.Data.RequestedOperation = DataPackageOperation.Copy;
                args.Data.SetStorageItems(new[] { _lastExportedFile }, false);
                args.DragUI.SetContentFromDataPackage();
            };
            Grid.SetColumn(dragBlock, 0);
            root.Children.Add(dragBlock);

            var actions = new StackPanel { Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
            var saveAsButton = new Button
            {
                Content = "Enregistrer sous",
                HorizontalAlignment = HorizontalAlignment.Center,
                Width = 160,
                Padding = new Thickness(0, 10, 0, 10),
                MinHeight = 40
            };
            saveAsButton.Click += async (_, _) => await SaveExportedFileAsAsync();
            actions.Children.Add(saveAsButton);

            var doneButton = new Button
            {
                Content = "Termin\u00e9",
                Style = SaveButton.Style,
                HorizontalAlignment = HorizontalAlignment.Center,
                Width = 160,
                Padding = new Thickness(0, 10, 0, 10),
                MinHeight = 40
            };
            doneButton.Click += (_, _) => _exportDialog?.Hide();
            actions.Children.Add(doneButton);
            Grid.SetColumn(actions, 1);
            root.Children.Add(actions);

            _exportDialog.Content = root;
        }

        private void ShowExportErrorContent(string message)
        {
            if (_exportDialog == null) return;

            _exportDialog.CloseButtonText = string.Empty;
            var root = new StackPanel { Spacing = 16 };
            root.Children.Add(new TextBlock
            {
                Text = "Export impossible",
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold
            });
            root.Children.Add(new TextBlock
            {
                Text = message,
                TextWrapping = TextWrapping.Wrap,
                Opacity = 0.72
            });

            var doneButton = new Button
            {
                Content = "Termin\u00e9",
                Style = SaveButton.Style,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                Padding = new Thickness(0, 12, 0, 12)
            };
            doneButton.Click += (_, _) => _exportDialog?.Hide();
            root.Children.Add(doneButton);

            _exportDialog.Content = root;
        }

        private async Task SaveExportedFileAsAsync()
        {
            if (_lastExportedFile == null || string.IsNullOrWhiteSpace(_lastExportedFile.Path)) return;

            var picker = new FileSavePicker
            {
                SuggestedStartLocation = PickerLocationId.VideosLibrary,
                SuggestedFileName = Path.GetFileNameWithoutExtension(_lastExportedFile.Name)
            };
            picker.FileTypeChoices.Add("Vid\u00e9o MP4", new List<string> { ".mp4" });

            if (App.MainWindow != null)
            {
                nint hwnd = WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow);
                WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            }

            StorageFile? destinationFile = await picker.PickSaveFileAsync();
            if (destinationFile == null) return;

            await _lastExportedFile.CopyAndReplaceAsync(destinationFile);
        }

        private static string FormatMegabytes(double megabytes)
        {
            return megabytes >= 10 ? $"{megabytes:0} Mo" : $"{megabytes:0.0} Mo";
        }

        private static string FormatFileSize(long bytes)
        {
            double megabytes = bytes / 1024.0 / 1024.0;
            return FormatMegabytes(megabytes);
        }

        private static Dictionary<int, AudioTrackManifestInfo> LoadAudioTrackManifest(string filePath)
        {
            var tracksByAudioIndex = new Dictionary<int, AudioTrackManifestInfo>();

            try
            {
                string packagePath = Path.ChangeExtension(filePath, ".fern");
                if (File.Exists(packagePath))
                {
                    return LoadFernPackageManifest(filePath, packagePath);
                }

                string manifestPath = Path.ChangeExtension(filePath, ".fern.json");
                if (!File.Exists(manifestPath)) return tracksByAudioIndex;
                string manifestDirectory = Path.GetDirectoryName(manifestPath) ?? string.Empty;

                using var document = JsonDocument.Parse(File.ReadAllText(manifestPath));
                if (!document.RootElement.TryGetProperty("tracks", out JsonElement tracks) ||
                    tracks.ValueKind != JsonValueKind.Array)
                {
                    return tracksByAudioIndex;
                }

                foreach (JsonElement track in tracks.EnumerateArray())
                {
                    if (!track.TryGetProperty("audioIndex", out JsonElement audioIndexElement) ||
                        !audioIndexElement.TryGetInt32(out int audioIndex) ||
                        audioIndex < 0)
                    {
                        continue;
                    }

                    if (!track.TryGetProperty("name", out JsonElement nameElement)) continue;

                    string? name = nameElement.GetString();
                    if (string.IsNullOrWhiteSpace(name)) continue;

                    double activeRatio = 0;
                    if (track.TryGetProperty("activeRatio", out JsonElement activeRatioElement))
                    {
                        activeRatioElement.TryGetDouble(out activeRatio);
                    }

                    double? volume = null;
                    if (track.TryGetProperty("volume", out JsonElement volumeElement) &&
                        volumeElement.TryGetDouble(out double volumeValue))
                    {
                        volume = Math.Clamp(volumeValue, 0, 100);
                    }

                    string sidecarPath = string.Empty;
                    if (track.TryGetProperty("sidecarFile", out JsonElement sidecarElement))
                    {
                        string? sidecarFile = sidecarElement.GetString();
                        if (!string.IsNullOrWhiteSpace(sidecarFile))
                        {
                            sidecarPath = Path.IsPathRooted(sidecarFile)
                                ? sidecarFile
                                : Path.Combine(manifestDirectory, sidecarFile);
                        }
                    }

                    tracksByAudioIndex[audioIndex] = new AudioTrackManifestInfo
                    {
                        Name = name,
                        SidecarPath = sidecarPath,
                        SidecarAudioIndex = 0,
                        ActiveRatio = Math.Clamp(activeRatio, 0, 1),
                        Volume = volume
                    };
                }
            }
            catch
            {
                tracksByAudioIndex.Clear();
            }

            return tracksByAudioIndex;
        }

        private static Dictionary<int, AudioTrackManifestInfo> LoadFernPackageManifest(string clipPath, string packagePath)
        {
            var tracksByAudioIndex = new Dictionary<int, AudioTrackManifestInfo>();

            using var stream = File.OpenRead(packagePath);
            Span<byte> magic = stackalloc byte[8];
            if (stream.Read(magic) != magic.Length || !magic.SequenceEqual("FERNPKG1"u8)) return tracksByAudioIndex;

            ulong jsonSize = ReadUInt64(stream);
            ulong audioSize = ReadUInt64(stream);
            if (jsonSize == 0 || jsonSize > 8 * 1024 * 1024 || audioSize == 0) return tracksByAudioIndex;

            byte[] jsonBytes = new byte[(int)jsonSize];
            if (stream.Read(jsonBytes, 0, jsonBytes.Length) != jsonBytes.Length) return tracksByAudioIndex;

            string audioBundlePath = EnsureFernPackageAudioBundle(clipPath, packagePath, stream, audioSize);
            if (string.IsNullOrWhiteSpace(audioBundlePath)) return tracksByAudioIndex;

            using var document = JsonDocument.Parse(jsonBytes);
            if (!document.RootElement.TryGetProperty("tracks", out JsonElement tracks) ||
                tracks.ValueKind != JsonValueKind.Array)
            {
                return tracksByAudioIndex;
            }

            foreach (JsonElement track in tracks.EnumerateArray())
            {
                if (!track.TryGetProperty("audioIndex", out JsonElement audioIndexElement) ||
                    !audioIndexElement.TryGetInt32(out int audioIndex) ||
                    audioIndex < 0)
                {
                    continue;
                }

                if (!track.TryGetProperty("name", out JsonElement nameElement)) continue;

                string? name = nameElement.GetString();
                if (string.IsNullOrWhiteSpace(name)) continue;

                double activeRatio = 0;
                if (track.TryGetProperty("activeRatio", out JsonElement activeRatioElement))
                {
                    activeRatioElement.TryGetDouble(out activeRatio);
                }

                double? volume = null;
                if (track.TryGetProperty("volume", out JsonElement volumeElement) &&
                    volumeElement.TryGetDouble(out double volumeValue))
                {
                    volume = Math.Clamp(volumeValue, 0, 100);
                }

                double lufsGain = 1.0;
                if (track.TryGetProperty("lufsGain", out JsonElement lufsGainElement) &&
                    lufsGainElement.TryGetDouble(out double lufsGainValue))
                {
                    lufsGain = Math.Max(lufsGainValue, 0.0);
                }

                tracksByAudioIndex[audioIndex] = new AudioTrackManifestInfo
                {
                    Name = name,
                    SidecarPath = audioBundlePath,
                    SidecarAudioIndex = audioIndex,
                    ActiveRatio = Math.Clamp(activeRatio, 0, 1),
                    Volume = volume,
                    LufsGain = lufsGain
                };
            }

            return tracksByAudioIndex;
        }

        private static ulong ReadUInt64(Stream stream)
        {
            Span<byte> buffer = stackalloc byte[8];
            if (stream.Read(buffer) != buffer.Length) return 0;
            return BitConverter.ToUInt64(buffer);
        }

        private static string EnsureFernPackageAudioBundle(string clipPath, string packagePath, Stream stream, ulong audioSize)
        {
            string cacheDirectory = Path.Combine(Path.GetTempPath(), "FernUI", "AudioCache");
            Directory.CreateDirectory(cacheDirectory);

            var packageInfo = new FileInfo(packagePath);
            string cacheName = $"{Path.GetFileNameWithoutExtension(clipPath)}_{packageInfo.LastWriteTimeUtc.Ticks}_{packageInfo.Length}.audio.mp4";
            string bundlePath = Path.Combine(cacheDirectory, cacheName);

            if (File.Exists(bundlePath) && (ulong)new FileInfo(bundlePath).Length == audioSize)
            {
                return bundlePath;
            }

            using var output = File.Create(bundlePath);
            byte[] buffer = new byte[64 * 1024];
            ulong remaining = audioSize;

            while (remaining > 0)
            {
                int toRead = (int)Math.Min((ulong)buffer.Length, remaining);
                int read = stream.Read(buffer, 0, toRead);
                if (read <= 0) return string.Empty;

                output.Write(buffer, 0, read);
                remaining -= (ulong)read;
            }

            return bundlePath;
        }

        private static void SaveAudioVolumesToFernPackage(string packagePath, IEnumerable<FernUI.Models.AudioTrack> audioTracks)
        {
            var tracksByAudioIndex = audioTracks.ToDictionary(track => track.AudioIndex);

            string tempPath = packagePath + ".tmp";

            using (var input = File.OpenRead(packagePath))
            {
                Span<byte> magic = stackalloc byte[8];
                if (input.Read(magic) != magic.Length || !magic.SequenceEqual("FERNPKG1"u8))
                {
                    return;
                }

                ulong jsonSize = ReadUInt64(input);
                ulong audioSize = ReadUInt64(input);
                if (jsonSize == 0 || jsonSize > 8 * 1024 * 1024 || audioSize == 0)
                {
                    return;
                }

                byte[] jsonBytes = new byte[(int)jsonSize];
                if (input.Read(jsonBytes, 0, jsonBytes.Length) != jsonBytes.Length)
                {
                    return;
                }

                JsonNode? rootNode = JsonNode.Parse(jsonBytes);
                if (rootNode is not JsonObject root ||
                    root["tracks"] is not JsonArray tracks)
                {
                    return;
                }

                foreach (JsonNode? trackNode in tracks)
                {
                    if (trackNode is not JsonObject trackObject) continue;
                    if (!TryGetInt(trackObject["audioIndex"], out int audioIndex)) continue;
                    if (!tracksByAudioIndex.TryGetValue(audioIndex, out var track)) continue;

                    trackObject["volume"] = Math.Round(Math.Clamp(track.Volume, 0, 100), 2);
                    if (Math.Abs(track.LufsGain - 1.0) > 0.001)
                    {
                        trackObject["lufsGain"] = Math.Round(Math.Max(track.LufsGain, 0.0), 4);
                    }
                    else
                    {
                        trackObject.Remove("lufsGain");
                    }
                }

                byte[] updatedJsonBytes = JsonSerializer.SerializeToUtf8Bytes(root, new JsonSerializerOptions
                {
                    WriteIndented = true
                });

                using var output = File.Open(tempPath, FileMode.Create, FileAccess.Write, FileShare.None);
                output.Write("FERNPKG1"u8);
                WriteUInt64(output, (ulong)updatedJsonBytes.Length);
                WriteUInt64(output, audioSize);
                output.Write(updatedJsonBytes);
                CopyExact(input, output, audioSize);
            }

            File.Move(tempPath, packagePath, overwrite: true);
        }

        private static bool TryGetInt(JsonNode? node, out int value)
        {
            value = 0;
            if (node == null) return false;

            try
            {
                value = node.GetValue<int>();
                return true;
            }
            catch
            {
                return false;
            }
        }

        private static void WriteUInt64(Stream stream, ulong value)
        {
            Span<byte> buffer = stackalloc byte[8];
            BitConverter.TryWriteBytes(buffer, value);
            stream.Write(buffer);
        }

        private static void CopyExact(Stream input, Stream output, ulong bytesToCopy)
        {
            byte[] buffer = new byte[64 * 1024];
            ulong remaining = bytesToCopy;

            while (remaining > 0)
            {
                int toRead = (int)Math.Min((ulong)buffer.Length, remaining);
                int read = input.Read(buffer, 0, toRead);
                if (read <= 0)
                {
                    throw new EndOfStreamException("Paquet Fern incomplet pendant la sauvegarde.");
                }

                output.Write(buffer, 0, read);
                remaining -= (ulong)read;
            }
        }

        private void CreateAudioPlayers()
        {
            DisposeAudioPlayers();

            if (_selectedClip == null || string.IsNullOrWhiteSpace(_selectedClip.FilePath))
            {
                return;
            }

            foreach (var track in AudioTracks)
            {
                if (string.IsNullOrWhiteSpace(track.SidecarPath) || !File.Exists(track.SidecarPath))
                {
                    continue;
                }

                int audioIndex = track.AudioIndex;
                _audioPlayerTrackSelections[audioIndex] = track.SidecarAudioIndex;
                var source = MediaSource.CreateFromUri(new Uri(track.SidecarPath));
                var item = new MediaPlaybackItem(source);

                var player = new MediaPlayer
                {
                    AutoPlay = false,
                    IsMuted = _isMuted
                };
                player.Source = item;

                _audioPlayers[audioIndex] = player;
                SelectAudioPlayerTrack(audioIndex);
            }

            ApplyAllAudioVolumes();
        }

        private bool SelectAudioPlayerTrack(int audioIndex)
        {
            if (!_audioPlayers.TryGetValue(audioIndex, out MediaPlayer? player)) return false;
            if (player.Source is not MediaPlaybackItem item) return false;

            int selectedTrackIndex = _audioPlayerTrackSelections.TryGetValue(audioIndex, out int value) ? value : 0;
            return SelectAudioTrack(item, selectedTrackIndex, audioIndex);
        }

        private void EnsureAudioTrackSelections()
        {
            foreach (int audioIndex in _audioPlayers.Keys.ToList())
            {
                SelectAudioPlayerTrack(audioIndex);
            }
        }

        private static bool SelectAudioTrack(MediaPlaybackItem item, int selectedTrackIndex, int audioIndex)
        {
            if (selectedTrackIndex < 0 || item.AudioTracks.Count <= selectedTrackIndex) return false;

            item.AudioTracks.SelectedIndex = selectedTrackIndex;
            System.Diagnostics.Debug.WriteLine($"Studio: lecteur audio {audioIndex} -> sidecar piste {item.AudioTracks.SelectedIndex}/{item.AudioTracks.Count}");
            return item.AudioTracks.SelectedIndex == selectedTrackIndex;
        }

        private void DisposeAudioPlayers()
        {
            foreach (var player in _audioPlayers.Values)
            {
                try
                {
                    player.Pause();
                    player.Source = null;
                    player.Dispose();
                }
                catch
                {
                    // Ignore cleanup failures from already-closed WinRT media objects.
                }
            }

            _audioPlayers.Clear();
            _audioPlayerTrackSelections.Clear();
        }

        private void PlayAudioPlayers()
        {
            EnsureAudioTrackSelections();
            ApplyAllAudioVolumes();
            foreach (var pair in _audioPlayers)
            {
                SelectAudioPlayerTrack(pair.Key);
                pair.Value.Play();
            }
        }

        private void PauseAudioPlayers()
        {
            foreach (var player in _audioPlayers.Values)
            {
                player.Pause();
            }
        }

        private void SeekAudioPlayers(TimeSpan position)
        {
            foreach (var pair in _audioPlayers)
            {
                SelectAudioPlayerTrack(pair.Key);
                pair.Value.PlaybackSession.Position = position;
            }
        }

        private void SynchronizeAudioPlayersToVideo(bool force)
        {
            if (_videoPlayer == null) return;

            TimeSpan position = _videoPlayer.PlaybackSession.Position;
            foreach (var pair in _audioPlayers)
            {
                MediaPlayer player = pair.Value;
                SelectAudioPlayerTrack(pair.Key);
                double driftMs = Math.Abs((player.PlaybackSession.Position - position).TotalMilliseconds);
                if (force || driftMs > 180)
                {
                    player.PlaybackSession.Position = position;
                }
            }
        }

        private void ApplyTrackVolume(FernUI.Models.AudioTrack track)
        {
            if (!_audioPlayers.TryGetValue(track.AudioIndex, out MediaPlayer? player)) return;

            double trackVolume = Math.Clamp(track.Volume / 100.0, 0.0, 1.0);
            double gain = trackVolume * track.LufsGain;
            player.IsMuted = _isMuted;
            player.Volume = _isMuted ? 0 : CalculateStudioPlayerVolume(gain);
        }

        private void ApplyAllAudioVolumes()
        {
            if (_videoPlayer != null)
            {
                if (_audioPlayers.Count > 0)
                {
                    _videoPlayer.IsMuted = true;
                    _videoPlayer.Volume = 0;
                }
                else
                {
                    _videoPlayer.IsMuted = _isMuted;
                    _videoPlayer.Volume = _isMuted ? 0 : CalculateStudioPlayerVolume(1.0);
                }
            }

            if (AudioTracks == null) return;

            foreach (var track in AudioTracks)
            {
                ApplyTrackVolume(track);
            }
        }

        private void PlaybackItem_AudioTracksChanged(MediaPlaybackItem sender, IVectorChangedEventArgs args)
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                if (_isTearingDownStudioSession || sender != _playbackItem)
                {
                    return;
                }

                DetectAudioTracks(sender);
            });
        }

        private void DetachPlaybackItemHandlers()
        {
            if (_playbackItem == null || _playbackItemAudioTracksChangedHandler == null)
            {
                return;
            }

            _playbackItem.AudioTracksChanged -= _playbackItemAudioTracksChangedHandler;
            _playbackItemAudioTracksChangedHandler = null;
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
            if (_videoPlayer == null) return;

            if (_videoPlayer.PlaybackSession.PlaybackState == MediaPlaybackState.Playing)
            {
                _videoPlayer.Pause();
                PauseAudioPlayers();
            }
            else
            {
                SynchronizeAudioPlayersToVideo(true);
                _videoPlayer.Play();
                PlayAudioPlayers();
            }
        }

        private void ProgressSlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            if (_isUpdatingSlider || _isSeeking) return;

            if (_videoPlayer != null && _videoPlayer.PlaybackSession.NaturalDuration.TotalSeconds > 0)
            {
                var timeSpan = TimeSpan.FromSeconds(e.NewValue);

                if (Math.Abs((timeSpan - _videoPlayer.PlaybackSession.Position).TotalMilliseconds) > 120)
                {
                    RequestSeek(timeSpan);
                }
            }
        }

        private void MuteButton_Click(object sender, RoutedEventArgs e)
        {
            _isMuted = !_isMuted;
            ApplyAllAudioVolumes();
            VolumeIcon.Glyph = _isMuted ? "\uE74F" : "\uE767";
        }

        private void VolumeSlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            if (_isUpdatingStudioVolumeControls) return;

            SetStudioVolumePercent(e.NewValue);
        }

        private void SetStudioVolumePercent(double percent, bool applyVolumes = true)
        {
            percent = Math.Clamp(percent, 0.0, 100.0);
            _studioVolume = percent / 100.0;

            _isUpdatingStudioVolumeControls = true;
            try
            {
                if (VolumeSlider != null && Math.Abs(VolumeSlider.Value - percent) > 0.01)
                {
                    VolumeSlider.Value = percent;
                }
            }
            finally
            {
                _isUpdatingStudioVolumeControls = false;
            }

            if (applyVolumes)
            {
                ApplyAllAudioVolumes();
            }
        }

        private void MasterVolumeSlider_ValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            if (_isUpdatingMasterVolumeControls) return;

            SetMasterDb(e.NewValue);
        }

        private void SetMasterDb(double db, bool applyVolumes = true)
        {
            _masterDb = ClampMasterDb(db);

            _isUpdatingMasterVolumeControls = true;
            try
            {
                SyncMasterDbControls();
            }
            finally
            {
                _isUpdatingMasterVolumeControls = false;
            }

            if (applyVolumes)
            {
                ApplyAllAudioVolumes();
            }
        }

        private double CurrentMasterGain => DbToGain(_masterDb);

        private void UpdateMasterDbText()
        {
            if (MasterVolumeText == null) return;

            MasterVolumeText.Text = $"{_masterDb:0.0} dB";
        }

        private void SyncMasterDbControls()
        {
            if (MasterVolumeSlider != null && Math.Abs(MasterVolumeSlider.Value - _masterDb) > 0.01)
            {
                MasterVolumeSlider.Value = _masterDb;
            }

            UpdateMasterDbText();
        }

        private double CalculateStudioPlayerVolume(double trackVolume)
        {
            double volume = _studioVolume * CurrentMasterGain * Math.Max(trackVolume, 0.0);
            return Math.Clamp(volume, 0.0, 1.0);
        }

        private static double DbToGain(double db)
        {
            return Math.Pow(10.0, ClampMasterDb(db) / 20.0);
        }

        private static double ClampMasterDb(double db)
        {
            return Math.Clamp(db, -40.0, 0.0);
        }

        private void StartLufsAnalysis()
        {
            CancelLufsAnalysis();
            HideLufsWarning();

            if (_selectedClip == null || string.IsNullOrWhiteSpace(_selectedClip.FilePath) || !File.Exists(_selectedClip.FilePath))
            {
                return;
            }

            var tracks = AudioTracks.ToList();
            if (tracks.Count == 0) return;

            var cancellation = new CancellationTokenSource();
            int generation = ++_lufsAnalysisGeneration;
            double masterGain = 1.0;
            _lufsAnalysisCancellation = cancellation;
            _lastLufsAnalysis = null;

            _ = AnalyzeLufsForCurrentClipAsync(_selectedClip.FilePath, tracks, masterGain, generation, cancellation);
        }

        private async Task AnalyzeLufsForCurrentClipAsync(
            string sourcePath,
            IReadOnlyList<FernUI.Models.AudioTrack> tracks,
            double masterGain,
            int generation,
            CancellationTokenSource cancellation)
        {
            try
            {
                StudioLufsAnalysis? analysis = await StudioLufsService.AnalyzeAsync(
                    sourcePath,
                    tracks,
                    masterGain,
                    cancellation.Token);

                DispatcherQueue.TryEnqueue(() =>
                {
                    if (_isTearingDownStudioSession || generation != _lufsAnalysisGeneration)
                    {
                        return;
                    }

                    _lastLufsAnalysis = analysis;

                    if (analysis == null)
                    {
                        HideLufsWarning();
                        return;
                    }

                    ShowLufsWarningIfNeeded(analysis);
                });
            }
            catch (OperationCanceledException)
            {
                // Expected when the user changes clip or leaves the Studio.
            }
            finally
            {
                DispatcherQueue.TryEnqueue(() =>
                {
                    if (_lufsAnalysisCancellation == cancellation)
                    {
                        _lufsAnalysisCancellation = null;
                    }
                });

                cancellation.Dispose();
            }
        }

        private void CancelLufsAnalysis()
        {
            _lufsAnalysisGeneration++;
            _lufsAnalysisCancellation?.Cancel();
            _lufsAnalysisCancellation = null;
            _lastLufsAnalysis = null;
        }

        private void ShowLufsWarningIfNeeded(StudioLufsAnalysis analysis)
        {
            if (analysis.AbsoluteErrorLu <= StudioLufsService.ToleranceLu)
            {
                HideLufsWarning();
                return;
            }

            LufsWarningText.Text = $"LUFS {analysis.IntegratedLufs:0.#}, cible {analysis.TargetLufs:0.#}.";
            LufsWarningPanel.Visibility = Visibility.Visible;
        }

        private void HideLufsWarning()
        {
            if (LufsWarningPanel != null)
            {
                LufsWarningPanel.Visibility = Visibility.Collapsed;
            }
        }

        private async void LufsAutoAdjustButton_Click(object sender, RoutedEventArgs e)
        {
            if (_selectedClip == null ||
                string.IsNullOrWhiteSpace(_selectedClip.FilePath) ||
                _lastLufsAnalysis == null ||
                _isNormalizingLufs)
            {
                return;
            }

            string sourcePath = _selectedClip.FilePath;
            var tracks = AudioTracks.ToList();
            StudioLufsAnalysis analysis = _lastLufsAnalysis;
            IsNormalizingLufs = true;
            LufsAutoAdjustButton.IsEnabled = false;
            LufsAutoAdjustButton.Content = "Ajustement...";
            LufsWarningText.Text = "Correction LUFS...";

            try
            {
                ReleaseMediaForSourceRewrite();
                await StudioLufsService.NormalizeClipAudioAsync(sourcePath, tracks, analysis, CancellationToken.None);
                SetMasterDb(0, false);
                InitializeMedia(sourcePath);
                HideLufsWarning();
            }
            catch (Exception ex)
            {
                if (File.Exists(sourcePath))
                {
                    InitializeMedia(sourcePath);
                }

                LufsWarningText.Text = $"Correction impossible: {ex.Message}";
                LufsWarningPanel.Visibility = Visibility.Visible;
            }
            finally
            {
                IsNormalizingLufs = false;
                LufsAutoAdjustButton.IsEnabled = true;
                LufsAutoAdjustButton.Content = "Ajuster";
            }
        }

        private async void EqualizeTracksToggle_Toggled(object sender, RoutedEventArgs e)
        {
            if (_selectedClip == null ||
                string.IsNullOrWhiteSpace(_selectedClip.FilePath) ||
                _isNormalizingLufs)
            {
                return;
            }

            if (EqualizeTracksToggle.IsOn)
            {
                IsNormalizingLufs = true;
                EqualizeTracksToggle.IsEnabled = false;
                try
                {
                    ReleaseMediaForSourceRewrite();
                    
                    var gains = new Dictionary<int, double>();
                    var tracksCopy = AudioTracks.ToList();
                    foreach (var track in tracksCopy)
                    {
                        var analysis = await StudioLufsService.AnalyzeAsync(_selectedClip.FilePath, new[] { track }, 1.0, CancellationToken.None);
                        if (analysis != null)
                        {
                            double correctionGain = Math.Pow(10.0, (-14.0 - analysis.IntegratedLufs) / 20.0);
                            double gain = Math.Max(correctionGain, 0.0);
                            gains[track.AudioIndex] = gain;
                            track.LufsGain = gain;
                        }
                    }

                    if (tracksCopy.Count > 1)
                    {
                        var mixAnalysis = await StudioLufsService.AnalyzeWithGainsAsync(_selectedClip.FilePath, tracksCopy, gains, CancellationToken.None);
                        if (mixAnalysis != null)
                        {
                            double mixCorrection = Math.Pow(10.0, (-14.0 - mixAnalysis.IntegratedLufs) / 20.0);
                            if (double.IsFinite(mixCorrection) && mixCorrection > 0)
                            {
                                foreach (var track in tracksCopy)
                                {
                                    gains[track.AudioIndex] *= mixCorrection;
                                    track.LufsGain = gains[track.AudioIndex];
                                }
                            }
                        }
                    }
                    
                    await StudioLufsService.ApplyTrackGainsAsync(_selectedClip.FilePath, tracksCopy, gains, CancellationToken.None);
                    
                    string packagePath = Path.ChangeExtension(_selectedClip.FilePath, ".fern");
                    if (File.Exists(packagePath)) SaveAudioVolumesToFernPackage(packagePath, tracksCopy);
                    
                    InitializeMedia(_selectedClip.FilePath);
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"Equalize fail: {ex.Message}");
                    InitializeMedia(_selectedClip.FilePath);
                }
                finally
                {
                    IsNormalizingLufs = false;
                    EqualizeTracksToggle.IsEnabled = true;
                }
            }
            else
            {
                bool changed = false;
                var undoGains = new Dictionary<int, double>();
                var tracksCopy = AudioTracks.ToList();
                foreach (var track in tracksCopy)
                {
                    if (Math.Abs(track.LufsGain - 1.0) > 0.001)
                    {
                        double undoGain = track.LufsGain > 0 ? 1.0 / track.LufsGain : 1.0;
                        undoGains[track.AudioIndex] = undoGain;
                        track.LufsGain = 1.0;
                        changed = true;
                    }
                    else
                    {
                        undoGains[track.AudioIndex] = 1.0;
                    }
                }
                
                if (changed)
                {
                    IsNormalizingLufs = true;
                    EqualizeTracksToggle.IsEnabled = false;
                    try
                    {
                        ReleaseMediaForSourceRewrite();
                        
                        var mixAnalysis = await StudioLufsService.AnalyzeWithGainsAsync(_selectedClip.FilePath, tracksCopy, undoGains, CancellationToken.None);
                        if (mixAnalysis != null)
                        {
                            double mixCorrection = Math.Pow(10.0, (-14.0 - mixAnalysis.IntegratedLufs) / 20.0);
                            if (double.IsFinite(mixCorrection) && mixCorrection > 0)
                            {
                                foreach (var track in tracksCopy)
                                {
                                    undoGains[track.AudioIndex] *= mixCorrection;
                                }
                            }
                        }
                        
                        await StudioLufsService.ApplyTrackGainsAsync(_selectedClip.FilePath, tracksCopy, undoGains, CancellationToken.None);
                        
                        string packagePath = System.IO.Path.ChangeExtension(_selectedClip.FilePath, ".fern");
                        if (File.Exists(packagePath)) SaveAudioVolumesToFernPackage(packagePath, tracksCopy);
                        
                        InitializeMedia(_selectedClip.FilePath);
                    }
                    catch (Exception ex)
                    {
                        System.Diagnostics.Debug.WriteLine($"Equalize undo fail: {ex.Message}");
                        InitializeMedia(_selectedClip.FilePath);
                    }
                    finally
                    {
                        IsNormalizingLufs = false;
                        EqualizeTracksToggle.IsEnabled = true;
                    }
                }
            }
        }

        private void ReleaseMediaForSourceRewrite()
        {
            CancelLufsAnalysis();
            PauseAudioPlayers();
            DisposeAudioPlayers();
            DetachPlaybackItemHandlers();
            _timer.Stop();

            if (_videoPlayer != null)
            {
                _videoPlayer.Pause();
                _videoPlayer.Source = null;
            }

            _playbackItem = null;
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
                ReleaseStudioSessionResources();
                Frame.GoBack();
                Frame.ForwardStack.Clear();
            }
        }

        private void ClipTitleTextBox_LostFocus(object sender, RoutedEventArgs e)
        {
            RenameClipFromStudio(ClipTitleTextBox.Text);
        }

        private void ClipTitleTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                RenameClipFromStudio(ClipTitleTextBox.Text);
                this.Focus(FocusState.Programmatic);
            }
        }

        private void RenameClipFromStudio(string newTitle)
        {
            if (_selectedClip == null || string.IsNullOrWhiteSpace(newTitle) || newTitle == _selectedClip.Title)
            {
                if (_selectedClip != null) ClipTitleTextBox.Text = _selectedClip.Title;
                return;
            }

            string oldPath = _selectedClip.FilePath;
            string dir = System.IO.Path.GetDirectoryName(oldPath) ?? string.Empty;
            string ext = System.IO.Path.GetExtension(oldPath);
            string newPath = System.IO.Path.Combine(dir, newTitle + ext);

            if (File.Exists(newPath))
            {
                ClipTitleTextBox.Text = _selectedClip.Title;
                return;
            }

            try
            {
                ReleaseMediaForSourceRewrite();

                File.Move(oldPath, newPath);
                string oldFern = System.IO.Path.ChangeExtension(oldPath, ".fern");
                string newFern = System.IO.Path.ChangeExtension(newPath, ".fern");
                if (File.Exists(oldFern)) File.Move(oldFern, newFern);

                _selectedClip.FilePath = newPath;
                _selectedClip.Title = newTitle;

                InitializeMedia(newPath);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Rename failed: {ex.Message}");
                ClipTitleTextBox.Text = _selectedClip.Title;
                InitializeMedia(oldPath);
            }
        }
    }
}
