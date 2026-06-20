using FernUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Microsoft.UI.Xaml.Navigation;
using Microsoft.UI.Xaml.Shapes;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Windows.Foundation;

namespace FernUI.Views
{
    public sealed partial class GalleryPage : Page
    {
        private List<ClipModel> _allClips = new();
        private List<string> _availableTags = new();

        private bool _isDragging = false;
        private bool _isDraggingRange = false;
        private double _startPointerX;
        private double _startHandleLeft;
        private double _startHandleRight;
        private Grid? _activeHandle = null;
        private bool _isTimelineVisible = false;

        private readonly List<FrameworkElement> _displayedCards = new();
        private int _selectedIndex = -1;

        private DateTime _minDate = DateTime.Today;
        private DateTime _maxDate = DateTime.Now;
        private CancellationTokenSource? _previewLoadCts;
        private readonly DispatcherTimer _folderRefreshDebounceTimer = new();
        private readonly Dictionary<FrameworkElement, (ClipModel Clip, PropertyChangedEventHandler Handler)> _previewSubscriptions = new();
        private FileSystemWatcher? _storageWatcher;
        private string _watchedStoragePath = string.Empty;
        private bool _isPageVisible;

        public GalleryPage()
        {
            InitializeComponent();
            NavigationCacheMode = NavigationCacheMode.Required;
            _folderRefreshDebounceTimer.Interval = TimeSpan.FromMilliseconds(1200);
            _folderRefreshDebounceTimer.Tick += FolderRefreshDebounceTimer_Tick;
            Loaded += GalleryPage_Loaded;
            Unloaded += GalleryPage_Unloaded;
        }

        private async void GalleryPage_Loaded(object sender, RoutedEventArgs e)
        {
            _isPageVisible = true;
            EnsureStorageWatcher();
            await LoadGalleryAsync();
        }

        private void GalleryPage_Unloaded(object sender, RoutedEventArgs e)
        {
            _isPageVisible = false;
            _previewLoadCts?.Cancel();
            _folderRefreshDebounceTimer.Stop();
            DisposeStorageWatcher();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            Frame.ForwardStack.Clear();
            MemoryCleanup.CollectNowAndAfter(DispatcherQueue);
        }

        private async void FolderRefreshDebounceTimer_Tick(object? sender, object e)
        {
            _folderRefreshDebounceTimer.Stop();
            if (!_isPageVisible) return;

            await LoadGalleryAsync(animateNewClips: true);
        }

        private void EnsureStorageWatcher()
        {
            string storagePath = SettingsService.NormalizeStoragePath(SettingsService.Instance.StoragePath);
            if (_storageWatcher != null &&
                string.Equals(_watchedStoragePath, storagePath, StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            DisposeStorageWatcher();

            try
            {
                Directory.CreateDirectory(storagePath);

                _storageWatcher = new FileSystemWatcher(storagePath)
                {
                    IncludeSubdirectories = false,
                    NotifyFilter = NotifyFilters.FileName |
                                   NotifyFilters.CreationTime |
                                   NotifyFilters.LastWrite |
                                   NotifyFilters.Size
                };

                _storageWatcher.Created += StorageWatcher_Changed;
                _storageWatcher.Changed += StorageWatcher_Changed;
                _storageWatcher.Deleted += StorageWatcher_Changed;
                _storageWatcher.Renamed += StorageWatcher_Renamed;
                _storageWatcher.EnableRaisingEvents = true;
                _watchedStoragePath = storagePath;
            }
            catch (IOException)
            {
                _watchedStoragePath = string.Empty;
            }
            catch (UnauthorizedAccessException)
            {
                _watchedStoragePath = string.Empty;
            }
        }

        private void DisposeStorageWatcher()
        {
            if (_storageWatcher == null) return;

            _storageWatcher.EnableRaisingEvents = false;
            _storageWatcher.Created -= StorageWatcher_Changed;
            _storageWatcher.Changed -= StorageWatcher_Changed;
            _storageWatcher.Deleted -= StorageWatcher_Changed;
            _storageWatcher.Renamed -= StorageWatcher_Renamed;
            _storageWatcher.Dispose();
            _storageWatcher = null;
            _watchedStoragePath = string.Empty;
        }

        private void StorageWatcher_Changed(object sender, FileSystemEventArgs e)
        {
            if (!ClipLibraryService.IsVideoFile(e.FullPath)) return;
            QueueFolderRefresh();
        }

        private void StorageWatcher_Renamed(object sender, RenamedEventArgs e)
        {
            if (!ClipLibraryService.IsVideoFile(e.FullPath) &&
                !ClipLibraryService.IsVideoFile(e.OldFullPath))
            {
                return;
            }

            QueueFolderRefresh();
        }

        private void QueueFolderRefresh()
        {
            if (!_isPageVisible) return;

            DispatcherQueue.TryEnqueue(() =>
            {
                if (!_isPageVisible) return;

                _folderRefreshDebounceTimer.Stop();
                _folderRefreshDebounceTimer.Start();
            });
        }

        private async Task LoadGalleryAsync(bool animateNewClips = false)
        {
            EnsureStorageWatcher();
            _previewLoadCts?.Cancel();
            _previewLoadCts = new CancellationTokenSource();
            CancellationToken cancellationToken = _previewLoadCts.Token;

            List<ClipModel> cachedClips = ClipLibraryService.GetCachedClips();
            if (_allClips.Count == 0 && cachedClips.Count > 0)
            {
                ApplyClipLibrary(cachedClips, rebuildCards: true);
            }

            List<ClipModel> loadedClips = await ClipLibraryService.LoadClipsAsync();
            if (cancellationToken.IsCancellationRequested) return;

            HashSet<string> newClipPaths = animateNewClips
                ? loadedClips
                    .Select(clip => clip.FilePath)
                    .Except(_allClips.Select(clip => clip.FilePath), StringComparer.OrdinalIgnoreCase)
                    .ToHashSet(StringComparer.OrdinalIgnoreCase)
                : new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            bool shouldRebuildCards = _displayedCards.Count == 0 || !HasSameClipOrder(_allClips, loadedClips);
            ApplyClipLibrary(loadedClips, shouldRebuildCards, newClipPaths);

            _ = LoadMissingPreviewsAsync(cancellationToken);
        }

        private void ApplyClipLibrary(List<ClipModel> clips, bool rebuildCards, HashSet<string>? newClipPaths = null)
        {
            _allClips = clips;
            _availableTags = BuildSearchTags(_allClips);
            UpdateDateRange();

            InitializeTimeline();

            if (rebuildCards)
                RefreshDisplayedClips(newClipPaths);
        }

        private async Task LoadMissingPreviewsAsync(CancellationToken cancellationToken)
        {
            try
            {
                foreach (ClipModel clip in _allClips.ToList())
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    await ClipLibraryService.EnsurePreviewAsync(clip, cancellationToken);
                    await Task.Yield();
                }
            }
            catch (OperationCanceledException)
            {
            }
        }

        private static bool HasSameClipOrder(IReadOnlyList<ClipModel> current, IReadOnlyList<ClipModel> next)
        {
            if (current.Count != next.Count) return false;

            for (int i = 0; i < current.Count; i++)
            {
                if (!string.Equals(current[i].FilePath, next[i].FilePath, StringComparison.OrdinalIgnoreCase) ||
                    current[i].CacheSignature != next[i].CacheSignature)
                {
                    return false;
                }
            }

            return true;
        }

        private void UpdateDateRange()
        {
            _minDate = DateTime.Today;
            _maxDate = DateTime.Now;

            if (_allClips.Count == 0) return;

            _minDate = _allClips.Min(c => c.CreationDate);
            _maxDate = _allClips.Max(c => c.CreationDate);
            if (_minDate == _maxDate) _minDate = _minDate.AddMinutes(-1);
        }

        private static List<string> BuildSearchTags(IEnumerable<ClipModel> clips)
        {
            return clips
                .SelectMany(clip => new[]
                {
                    clip.Title,
                    clip.Resolution,
                    clip.CreationDate.Year.ToString()
                })
                .Where(value => !string.IsNullOrWhiteSpace(value))
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .OrderBy(value => value)
                .ToList();
        }

        private void InitializeTimeline()
        {
            if (_allClips.Count == 0) return;

            double width = TimelineCanvas.ActualWidth;
            if (width <= 0) return;

            Canvas.SetLeft(StartHandle, -20);
            Canvas.SetLeft(EndHandle, width - 20);

            UpdateTimelineVisuals();
            DrawDensityCurve();
        }

        private void UpdateTimelineVisuals()
        {
            if (_allClips.Count == 0) return;

            double width = TimelineCanvas.ActualWidth;
            double startX = Canvas.GetLeft(StartHandle) + 20;
            double endX = Canvas.GetLeft(EndHandle) + 20;

            double visualStart = Math.Min(startX, endX);
            double visualEnd = Math.Max(startX, endX);

            SelectedRangeContainer.Margin = new Thickness(visualStart, 0, 0, 0);
            SelectedRangeContainer.Width = Math.Max(0, visualEnd - visualStart);

            string dateFormat = "dd MMMM yyyy";
            StartDateText.Text = GetDateFromX(visualStart).ToString(dateFormat);
            EndDateText.Text = GetDateFromX(visualEnd).ToString(dateFormat);

            PositionTimelineDateLabel(StartDateText, visualStart, width);
            PositionTimelineDateLabel(EndDateText, visualEnd, width);
        }

        private static void PositionTimelineDateLabel(TextBlock label, double pointX, double canvasWidth)
        {
            double labelWidth = label.Width;
            double left = pointX - labelWidth / 2;
            double clampedLeft = Math.Max(0, Math.Min(left, Math.Max(0, canvasWidth - labelWidth)));
            Canvas.SetLeft(label, clampedLeft);
        }

        private DateTime GetDateFromX(double x)
        {
            if (_allClips.Count == 0 || TimelineCanvas.ActualWidth <= 0) return _maxDate;

            double ratio = x / TimelineCanvas.ActualWidth;
            long ticksRange = (_maxDate - _minDate).Ticks;
            return _minDate.AddTicks((long)(ticksRange * ratio));
        }

        private void DrawDensityCurve()
        {
            GraphCanvas.Children.Clear();
            if (_allClips.Count == 0) return;

            double width = GraphCanvas.ActualWidth;
            double height = GraphCanvas.ActualHeight;
            if (width <= 0 || height <= 0) return;

            int segments = 40;
            var points = new List<Point>();
            long totalTicks = (_maxDate - _minDate).Ticks;
            if (totalTicks <= 0) return;

            for (int i = 0; i <= segments; i++)
            {
                double x = (width / segments) * i;
                DateTime segStart = _minDate.AddTicks((totalTicks / segments) * i);
                DateTime segEnd = i == segments ? _maxDate : _minDate.AddTicks((totalTicks / segments) * (i + 1));

                int count = _allClips.Count(c => c.CreationDate >= segStart && c.CreationDate < segEnd);
                double normalizedCount = Math.Min(count, 4);
                double y = height - (normalizedCount * (height / 5.0)) - 10;
                
                if (!double.IsNaN(x) && !double.IsNaN(y) && !double.IsInfinity(x) && !double.IsInfinity(y))
                {
                    points.Add(new Point(x, y));
                }
            }

            if (points.Count < 2) return;

            List<Point> smoothedPoints = BuildInterpolatedCurvePoints(SmoothCurvePoints(points));
            // Filtrer les éventuels NaN issus de l'interpolation
            smoothedPoints = smoothedPoints.Where(p => !double.IsNaN(p.X) && !double.IsNaN(p.Y)).ToList();
            
            if (smoothedPoints.Count < 2) return;

            Brush accentBrush = (Brush)Application.Current.Resources["SystemControlHighlightAccentBrush"];

            // Créer deux géométries distinctes pour éviter les erreurs de plage WinRT
            PathGeometry CreateGeometry()
            {
                var pg = new PathGeometry();
                var figure = new PathFigure { StartPoint = smoothedPoints[0], IsClosed = false, IsFilled = false };
                var polySegment = new PolyLineSegment();
                foreach (var p in smoothedPoints.Skip(1)) polySegment.Points.Add(p);
                figure.Segments.Add(polySegment);
                pg.Figures.Add(figure);
                return pg;
            }

            var softTrail = new Microsoft.UI.Xaml.Shapes.Path
            {
                Data = CreateGeometry(),
                Stroke = accentBrush,
                StrokeThickness = 3,
                Opacity = 0.12,
                StrokeLineJoin = PenLineJoin.Round
            };

            var curve = new Microsoft.UI.Xaml.Shapes.Path
            {
                Data = CreateGeometry(),
                Stroke = accentBrush,
                StrokeThickness = 1, // Largeur réduite pour plus de finesse
                Opacity = 0.7,
                StrokeLineJoin = PenLineJoin.Round
            };

            GraphCanvas.Children.Add(softTrail);
            GraphCanvas.Children.Add(curve);
        }

        private static List<Point> SmoothCurvePoints(IReadOnlyList<Point> points)
        {
            if (points.Count <= 2) return points.ToList();
            var smoothed = new List<Point>(points.Count) { points[0] };
            for (int i = 1; i < points.Count - 1; i++)
            {
                smoothed.Add(new Point(points[i].X, points[i - 1].Y * 0.2 + points[i].Y * 0.6 + points[i + 1].Y * 0.2));
            }
            smoothed.Add(points[^1]);
            return smoothed;
        }

        private static List<Point> BuildInterpolatedCurvePoints(IReadOnlyList<Point> points)
        {
            if (points.Count <= 2) return points.ToList();
            var interpolated = new List<Point> { points[0] };
            for (int i = 0; i < points.Count - 1; i++)
            {
                Point p0 = i > 0 ? points[i - 1] : points[i];
                Point p1 = points[i];
                Point p2 = points[i + 1];
                Point p3 = i + 2 < points.Count ? points[i + 2] : p2;
                for (int step = 1; step <= 5; step++)
                {
                    double t = step / 5.0;
                    double t2 = t * t;
                    double t3 = t2 * t;
                    double x = 0.5 * ((2 * p1.X) + (-p0.X + p2.X) * t + (2 * p0.X - 5 * p1.X + 4 * p2.X - p3.X) * t2 + (-p0.X + 3 * p1.X - 3 * p2.X + p3.X) * t3);
                    double y = 0.5 * ((2 * p1.Y) + (-p0.Y + p2.Y) * t + (2 * p0.Y - 5 * p1.Y + 4 * p2.Y - p3.Y) * t2 + (-p0.Y + 3 * p1.Y - 3 * p2.Y + p3.Y) * t3);
                    interpolated.Add(new Point(x, y));
                }
            }
            return interpolated;
        }

        private void RefreshDisplayedClips(HashSet<string>? newClipPaths = null)
        {
            List<ClipModel> clips = GetFilteredClips();
            if (newClipPaths is { Count: > 0 })
            {
                DisplayClipsWithInsertionAnimation(clips, newClipPaths);
                return;
            }

            DisplayClips(clips);
        }

        private void DisplayClips(IEnumerable<ClipModel> clipsToDisplay)
        {
            List<ClipModel> clips = clipsToDisplay?.ToList() ?? new List<ClipModel>();

            ClearPreviewSubscriptions();
            _displayedCards.Clear();
            _selectedIndex = -1;

            Column0.Children.Clear();
            Column1.Children.Clear();
            Column2.Children.Clear();

            int index = 0;
            foreach (var clip in clips)
            {
                var card = CreateClipCard(clip);
                _displayedCards.Add(card);

                int column = index % 3;
                if (column == 0) Column0.Children.Add(card);
                else if (column == 1) Column1.Children.Add(card);
                else Column2.Children.Add(card);
                index++;
            }

            EmptyState.Visibility = clips.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            EmptyStatePathText.Text = SettingsService.NormalizeStoragePath(SettingsService.Instance.StoragePath);
        }

        private void DisplayClipsWithInsertionAnimation(List<ClipModel> clips, HashSet<string> newClipPaths)
        {
            var existingCardsByPath = _displayedCards
                .Where(card => card.DataContext is ClipModel clip && !string.IsNullOrWhiteSpace(clip.FilePath))
                .GroupBy(card => ((ClipModel)card.DataContext).FilePath, StringComparer.OrdinalIgnoreCase)
                .ToDictionary(group => group.Key, group => group.First(), StringComparer.OrdinalIgnoreCase);

            var targetCards = new List<FrameworkElement>(clips.Count);
            var newCardsToAnimate = new List<FrameworkElement>();
            foreach (ClipModel clip in clips)
            {
                FrameworkElement card;
                if (existingCardsByPath.TryGetValue(clip.FilePath, out FrameworkElement? existingCard) &&
                    existingCard.DataContext is ClipModel existingClip &&
                    existingClip.CacheSignature == clip.CacheSignature)
                {
                    card = existingCard;
                    existingCardsByPath.Remove(clip.FilePath);
                }
                else
                {
                    card = CreateClipCard(clip);
                    if (newClipPaths.Contains(clip.FilePath))
                    {
                        newCardsToAnimate.Add(card);
                    }
                }

                targetCards.Add(card);
            }

            foreach (FrameworkElement staleCard in existingCardsByPath.Values)
            {
                RemoveClipCard(staleCard);
            }

            ArrangeCards(targetCards);
            _displayedCards.Clear();
            _displayedCards.AddRange(targetCards);
            _selectedIndex = -1;

            foreach (FrameworkElement newCard in newCardsToAnimate)
            {
                AnimateNewClipCard(newCard);
            }

            EmptyState.Visibility = clips.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            EmptyStatePathText.Text = SettingsService.NormalizeStoragePath(SettingsService.Instance.StoragePath);
        }

        private FrameworkElement CreateClipCard(ClipModel clip)
        {
            var template = (DataTemplate)Resources["ClipCardTemplate"];
            var card = (FrameworkElement)template.LoadContent();
            card.DataContext = clip;
            InitializePreviewAnimation(card, clip);
            return card;
        }

        private void ArrangeCards(IReadOnlyList<FrameworkElement> cards)
        {
            for (int i = 0; i < cards.Count; i++)
            {
                StackPanel column = GetClipColumn(i % 3);
                int columnIndex = i / 3;
                MoveCardToColumn(cards[i], column, columnIndex);
            }
        }

        private StackPanel GetClipColumn(int column)
        {
            return column switch
            {
                0 => Column0,
                1 => Column1,
                _ => Column2
            };
        }

        private static void MoveCardToColumn(FrameworkElement card, StackPanel column, int targetIndex)
        {
            if (card.Parent is StackPanel currentColumn)
            {
                int currentIndex = currentColumn.Children.IndexOf(card);
                if (currentColumn == column && currentIndex == targetIndex) return;

                currentColumn.Children.Remove(card);
            }

            column.Children.Insert(Math.Min(targetIndex, column.Children.Count), card);
        }

        private void RemoveClipCard(FrameworkElement card)
        {
            RemovePreviewSubscription(card);

            if (card.Parent is StackPanel parent)
            {
                parent.Children.Remove(card);
            }
        }

        private void InitializePreviewAnimation(FrameworkElement card, ClipModel clip)
        {
            if (card.FindName("PreviewImageControl") is not Image previewImage) return;

            previewImage.Opacity = clip.PreviewImage == null ? 0 : 1;

            PropertyChangedEventHandler handler = (_, args) =>
            {
                if (args.PropertyName != nameof(ClipModel.PreviewImage) || clip.PreviewImage == null) return;

                DispatcherQueue.TryEnqueue(() =>
                {
                    if (_displayedCards.Contains(card))
                        AnimatePreviewImage(previewImage);
                });
            };

            clip.PropertyChanged += handler;
            _previewSubscriptions[card] = (clip, handler);
        }

        private void ClearPreviewSubscriptions()
        {
            foreach (var subscription in _previewSubscriptions.Values)
            {
                subscription.Clip.PropertyChanged -= subscription.Handler;
            }

            _previewSubscriptions.Clear();
        }

        private void RemovePreviewSubscription(FrameworkElement card)
        {
            if (!_previewSubscriptions.Remove(card, out var subscription)) return;

            subscription.Clip.PropertyChanged -= subscription.Handler;
        }

        private void Page_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (_displayedCards.Count == 0) return;

            if (FocusManager.GetFocusedElement(XamlRoot) is AutoSuggestBox ||
                FocusManager.GetFocusedElement(XamlRoot) is TextBox) return;

            bool isNavKey = true;
            int newIndex = _selectedIndex;

            switch (e.Key)
            {
                case Windows.System.VirtualKey.Right:
                    newIndex = _selectedIndex < 0 ? 0 : Math.Min(_displayedCards.Count - 1, _selectedIndex + 1);
                    break;
                case Windows.System.VirtualKey.Left:
                    newIndex = _selectedIndex < 0 ? 0 : Math.Max(0, _selectedIndex - 1);
                    break;
                case Windows.System.VirtualKey.Down:
                    newIndex = _selectedIndex < 0 ? 0 : Math.Min(_displayedCards.Count - 1, _selectedIndex + 3);
                    break;
                case Windows.System.VirtualKey.Up:
                    newIndex = _selectedIndex < 0 ? 0 : Math.Max(0, _selectedIndex - 3);
                    break;
                case Windows.System.VirtualKey.Enter:
                    if (_selectedIndex >= 0) Frame.Navigate(typeof(StudioPage), _displayedCards[_selectedIndex].DataContext);
                    return;
                case Windows.System.VirtualKey.Escape:
                    if (_selectedIndex >= 0) UpdateSelection(-1);
                    return;
                default:
                    isNavKey = false;
                    break;
            }

            if (isNavKey)
            {
                e.Handled = true;
                if (newIndex != _selectedIndex) UpdateSelection(newIndex);
            }
        }

        private void UpdateSelection(int index)
        {
            if (_selectedIndex >= 0 && _selectedIndex < _displayedCards.Count)
                SetSelectionVisual(_displayedCards[_selectedIndex], false);

            _selectedIndex = index;

            if (_selectedIndex >= 0 && _selectedIndex < _displayedCards.Count)
            {
                var card = _displayedCards[_selectedIndex];
                SetSelectionVisual(card, true);
                card.Focus(FocusState.Keyboard);
                card.StartBringIntoView();
            }
        }

        private static void SetSelectionVisual(FrameworkElement card, bool isSelected)
        {
            if (card.FindName("SelectionBorder") is Border border)
                border.Opacity = isSelected ? 1 : 0;
        }

        private void DateFilterButton_Click(object sender, RoutedEventArgs e)
        {
            if (_allClips.Count == 0) return;

            _isTimelineVisible = !_isTimelineVisible;
            if (_isTimelineVisible)
            {
                ShowTimelineStoryboard.Begin();
                InitializeTimeline();
            }
            else
            {
                HideTimelineStoryboard.Begin();
            }
        }

        private void Handle_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (sender is Grid handle)
            {
                _isDragging = true;
                _activeHandle = handle;
                handle.CapturePointer(e.Pointer);
            }
        }

        private void Handle_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (_isDragging && _activeHandle != null)
            {
                var point = e.GetCurrentPoint(TimelineCanvas);
                double x = point.Position.X;
                double newLeft = Math.Max(-20, Math.Min(x - 20, TimelineCanvas.ActualWidth - 20));
                Canvas.SetLeft(_activeHandle, newLeft);
                UpdateTimelineVisuals();
            }
        }

        private void Handle_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (_isDragging)
            {
                _isDragging = false;
                _activeHandle?.ReleasePointerCapture(e.Pointer);
                _activeHandle = null;
                FilterClips();
            }
        }

        private void RangeBar_PointerPressed(object sender, PointerRoutedEventArgs e)
        {
            _isDraggingRange = true;
            var point = e.GetCurrentPoint(TimelineCanvas);
            _startPointerX = point.Position.X;
            _startHandleLeft = Canvas.GetLeft(StartHandle);
            _startHandleRight = Canvas.GetLeft(EndHandle);
            SelectedRangeContainer.CapturePointer(e.Pointer);
        }

        private void RangeBar_PointerMoved(object sender, PointerRoutedEventArgs e)
        {
            if (_isDraggingRange)
            {
                var point = e.GetCurrentPoint(TimelineCanvas);
                double deltaX = point.Position.X - _startPointerX;
                double width = TimelineCanvas.ActualWidth;
                double newLeft = _startHandleLeft + deltaX;
                double newRight = _startHandleRight + deltaX;

                if (newLeft >= -20 && newRight <= width - 20)
                {
                    Canvas.SetLeft(StartHandle, newLeft);
                    Canvas.SetLeft(EndHandle, newRight);
                    UpdateTimelineVisuals();
                }
            }
        }

        private void RangeBar_PointerReleased(object sender, PointerRoutedEventArgs e)
        {
            if (_isDraggingRange)
            {
                _isDraggingRange = false;
                SelectedRangeContainer.ReleasePointerCapture(e.Pointer);
                FilterClips();
            }
        }

        private void FilterClips()
        {
            DisplayClips(GetFilteredClips());
        }

        private List<ClipModel> GetFilteredClips()
        {
            IEnumerable<ClipModel> filtered = _allClips;

            if (_allClips.Count > 0 && TimelineCanvas.ActualWidth > 0)
            {
                double startX = Canvas.GetLeft(StartHandle) + 20;
                double endX = Canvas.GetLeft(EndHandle) + 20;
                double visualStart = Math.Min(startX, endX);
                double visualEnd = Math.Max(startX, endX);

                DateTime dateStart = GetDateFromX(visualStart);
                DateTime dateEnd = GetDateFromX(visualEnd);
                filtered = filtered.Where(c => c.CreationDate >= dateStart && c.CreationDate <= dateEnd);
            }

            string query = SearchBox.Text.Trim().ToLowerInvariant();
            if (!string.IsNullOrWhiteSpace(query))
            {
                var terms = query.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                filtered = filtered.Where(c => terms.All(t =>
                    c.Title.Contains(t, StringComparison.OrdinalIgnoreCase) ||
                    c.Resolution.Contains(t, StringComparison.OrdinalIgnoreCase)));
            }

            return filtered.ToList();
        }

        private void SearchBox_TextChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args)
        {
            sender.ItemsSource = _availableTags
                .Where(t => t.Contains(sender.Text, StringComparison.OrdinalIgnoreCase))
                .Take(10)
                .ToList();
            FilterClips();
        }

        private void SearchBox_QuerySubmitted(AutoSuggestBox sender, AutoSuggestBoxQuerySubmittedEventArgs args) => FilterClips();

        private static void AnimatePreviewImage(Image previewImage)
        {
            previewImage.Opacity = 0;

            var fadeInAnimation = new DoubleAnimation
            {
                From = 0,
                To = 1,
                Duration = TimeSpan.FromMilliseconds(220),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
            };

            Storyboard.SetTarget(fadeInAnimation, previewImage);
            Storyboard.SetTargetProperty(fadeInAnimation, nameof(UIElement.Opacity));

            var storyboard = new Storyboard();
            storyboard.Children.Add(fadeInAnimation);
            storyboard.Begin();
        }

        private static void AnimateNewClipCard(FrameworkElement card)
        {
            card.Opacity = 0;

            if (card.RenderTransform is not CompositeTransform transform)
            {
                transform = new CompositeTransform();
                card.RenderTransform = transform;
            }

            transform.ScaleX = 0.96;
            transform.ScaleY = 0.96;
            transform.TranslateY = -22;

            var storyboard = new Storyboard();
            storyboard.Children.Add(CreateDoubleAnimation(card, nameof(UIElement.Opacity), 0, 1, 280));
            storyboard.Children.Add(CreateDoubleAnimation(transform, nameof(CompositeTransform.ScaleX), 0.96, 1, 420));
            storyboard.Children.Add(CreateDoubleAnimation(transform, nameof(CompositeTransform.ScaleY), 0.96, 1, 420));
            storyboard.Children.Add(CreateDoubleAnimation(transform, nameof(CompositeTransform.TranslateY), -22, 0, 420));

            if (card.FindName("NewClipHighlightBorder") is Border highlight)
            {
                highlight.Opacity = 1;
                storyboard.Children.Add(CreateDoubleAnimation(highlight, nameof(UIElement.Opacity), 1, 0, 950));
            }

            storyboard.Begin();
        }

        private static DoubleAnimation CreateDoubleAnimation(
            DependencyObject target,
            string property,
            double from,
            double to,
            int durationMs)
        {
            var animation = new DoubleAnimation
            {
                From = from,
                To = to,
                Duration = TimeSpan.FromMilliseconds(durationMs),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
            };

            Storyboard.SetTarget(animation, target);
            Storyboard.SetTargetProperty(animation, property);
            return animation;
        }

        private void CardRoot_PointerEntered(object sender, PointerRoutedEventArgs e)
        {
        }

        private void CardRoot_PointerExited(object sender, PointerRoutedEventArgs e)
        {
        }

        private void CardRoot_Tapped(object sender, TappedRoutedEventArgs e)
        {
            if (sender is FrameworkElement card && card.DataContext is ClipModel clip)
            {
                Frame.Navigate(typeof(StudioPage), clip);
            }
        }

        private void TitleTextBox_Tapped(object sender, TappedRoutedEventArgs e)
        {
            e.Handled = true;
        }

        private void TitleTextBox_LostFocus(object sender, RoutedEventArgs e)
        {
            if (sender is TextBox textBox && textBox.DataContext is ClipModel clip)
            {
                RenameClipFromGallery(clip, textBox, textBox.Text);
            }
        }

        private void TitleTextBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == Windows.System.VirtualKey.Enter)
            {
                if (sender is TextBox textBox && textBox.DataContext is ClipModel clip)
                {
                    RenameClipFromGallery(clip, textBox, textBox.Text);
                    this.Focus(FocusState.Programmatic);
                }
            }
        }

        private void RenameClipFromGallery(ClipModel clip, TextBox textBox, string newTitle)
        {
            if (string.IsNullOrWhiteSpace(newTitle) || newTitle == clip.Title)
            {
                textBox.Text = clip.Title;
                return;
            }

            string oldPath = clip.FilePath;
            string dir = System.IO.Path.GetDirectoryName(oldPath) ?? string.Empty;
            string ext = System.IO.Path.GetExtension(oldPath);
            string newPath = System.IO.Path.Combine(dir, newTitle + ext);

            if (File.Exists(newPath))
            {
                textBox.Text = clip.Title;
                return;
            }

            try
            {
                File.Move(oldPath, newPath);
                string oldFern = System.IO.Path.ChangeExtension(oldPath, ".fern");
                string newFern = System.IO.Path.ChangeExtension(newPath, ".fern");
                if (File.Exists(oldFern)) File.Move(oldFern, newFern);

                clip.FilePath = newPath;
                clip.Title = newTitle;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Rename failed: {ex.Message}");
                textBox.Text = clip.Title;
            }
        }
    }
}
