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
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using Windows.Foundation;
using Windows.Foundation.Collections;

namespace FernUI.Views
{
    public sealed partial class StudioPage : Page
    {
        public ObservableCollection<FernUI.Models.AudioTrack> AudioTracks { get; } = new();
        private ClipModel? _selectedClip;
        private DispatcherTimer _timer;
        private DispatcherTimer _singleTapTimer;
        private DispatcherTimer _controlsHideTimer;
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
        private double _masterVolume = 1.0;
        private bool _isMuted;
        private bool _isTearingDownStudioSession;
        private TypedEventHandler<MediaPlaybackItem, IVectorChangedEventArgs>? _playbackItemAudioTracksChangedHandler;

        private sealed class AudioTrackManifestInfo
        {
            public string Name { get; set; } = string.Empty;
            public string SidecarPath { get; set; } = string.Empty;
            public int SidecarAudioIndex { get; set; }
            public double ActiveRatio { get; set; }
        }

        public StudioPage()
        {
            this.InitializeComponent();

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
                if (lowerName.Contains("mic")) icon = "\uE720";
                else if (lowerName.Contains("game") || lowerName.Contains("jeu") || lowerName.Contains("system")) icon = "\uE7FC";
                else if (lowerName.Contains("discord") || lowerName.Contains("chat")) icon = "\uE191";

                var model = new FernUI.Models.AudioTrack 
                { 
                    AudioIndex = i,
                    Name = name, 
                    Icon = icon, 
                    Volume = isVoiceMeeter ? 0 : 100, 
                    SidecarPath = manifestInfo?.SidecarPath ?? string.Empty,
                    SidecarAudioIndex = manifestInfo?.SidecarAudioIndex ?? 0,
                    ActiveRatio = manifestInfo?.ActiveRatio ?? 0,
                    PeakLevel = 0 
                };
                
                model.OnVolumeChanged += Track_OnVolumeChanged;
                detectedTracks.Add(model);
            }

            foreach (var model in detectedTracks
                .OrderBy(track => track.Name.Contains("voicemeeter", StringComparison.OrdinalIgnoreCase))
                .ThenByDescending(track => track.ActiveRatio)
                .ThenBy(track => track.Name, StringComparer.OrdinalIgnoreCase))
            {
                AudioTracks.Add(model);
            }
            
            CreateAudioPlayers();
            ApplyAllAudioVolumes();
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
                        ActiveRatio = Math.Clamp(activeRatio, 0, 1)
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

                tracksByAudioIndex[audioIndex] = new AudioTrackManifestInfo
                {
                    Name = name,
                    SidecarPath = audioBundlePath,
                    SidecarAudioIndex = audioIndex,
                    ActiveRatio = Math.Clamp(activeRatio, 0, 1)
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
            player.IsMuted = _isMuted;
            player.Volume = _isMuted ? 0 : Math.Clamp(_masterVolume * trackVolume, 0.0, 1.0);
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
                    _videoPlayer.Volume = _isMuted ? 0 : _masterVolume;
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
            _masterVolume = e.NewValue / 100.0;
            ApplyAllAudioVolumes();
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
    }
}
