using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using System;
using System.Collections.Generic;
using System.Linq;
using FernUI.Models;
using Windows.Foundation;

namespace FernUI.Views
{
    public sealed partial class GalleryPage : Page
    {
        private const string SampleVideo = "https://raw.githubusercontent.com/microsoft/Windows-universal-samples/master/Samples/MediaPlayback/CS/Shared/Assets/SampleVideo.mp4";
        
        private List<ClipModel> _allClips = new();
        private List<string> _availableTags = new() { "CS2", "Cyberpunk", "Overwatch 2", "Elden Ring", "LoL", "Clutch", "Pentakill", "Sniper", "Funny", "Epic" };

        private bool _isDragging = false;
        private bool _isDraggingRange = false;
        private double _startPointerX;
        private double _startHandleLeft;
        private double _startHandleRight;
        private Grid? _activeHandle = null;
        private bool _isTimelineVisible = false;

        private List<FrameworkElement> _displayedCards = new();
        private int _selectedIndex = -1;

        private DateTime _minDate;
        private DateTime _maxDate = DateTime.Now;

        public GalleryPage()
        {
            this.InitializeComponent();
            InitializeData();
            
            this.Loaded += (s, e) => {
                InitializeTimeline();
                DisplayClips(_allClips);
            };
        }

        private void InitializeData()
        {
            var now = DateTime.Now;
            _allClips = new List<ClipModel>
            {
                new ClipModel { Title = "Epic Win - Sniper Clutch", Game = "CS2", Date = "10 min ago", CreationDate = now.AddMinutes(-10), Duration = "00:30", CardHeight = 280, VideoUrl = SampleVideo },
                new ClipModel { Title = "Fail Hilarious Physics", Game = "Cyberpunk", Date = "1h ago", CreationDate = now.AddHours(-1), Duration = "00:15", CardHeight = 200, VideoUrl = SampleVideo },
                new ClipModel { Title = "Quad Kill Ulti", Game = "Overwatch 2", Date = "Today", CreationDate = now.AddHours(-4), Duration = "00:45", CardHeight = 320, VideoUrl = SampleVideo },
                new ClipModel { Title = "World Record? Maybe.", Game = "Elden Ring", Date = "Yesterday", CreationDate = now.AddDays(-1), Duration = "01:00", CardHeight = 240, VideoUrl = SampleVideo },
                new ClipModel { Title = "Pentakill Midlane", Game = "LoL", Date = "2 days ago", CreationDate = now.AddDays(-2), Duration = "00:55", CardHeight = 180, VideoUrl = SampleVideo },
                new ClipModel { Title = "Insane Drift", Game = "Forza", Date = "3 days ago", CreationDate = now.AddDays(-3), Duration = "00:20", CardHeight = 300, VideoUrl = SampleVideo },
                new ClipModel { Title = "Funny Glitch", Game = "Starfield", Date = "A week ago", CreationDate = now.AddDays(-7), Duration = "00:12", CardHeight = 220, VideoUrl = SampleVideo },
                new ClipModel { Title = "Boss Fight No Hit", Game = "Sekiro", Date = "2 weeks ago", CreationDate = now.AddDays(-14), Duration = "02:30", CardHeight = 350, VideoUrl = SampleVideo },
                new ClipModel { Title = "Clean Ace", Game = "Valorant", Date = "Last month", CreationDate = now.AddDays(-30), Duration = "00:40", CardHeight = 260, VideoUrl = SampleVideo }
            };

            _minDate = _allClips.Min(c => c.CreationDate);
        }

        private void InitializeTimeline()
        {
            double width = TimelineCanvas.ActualWidth;
            if (width <= 0) return;

            Canvas.SetLeft(StartHandle, -20);
            Canvas.SetLeft(EndHandle, width - 20);

            UpdateTimelineVisuals();
            DrawDensityCurve();
        }

        private void UpdateTimelineVisuals()
        {
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
        }

        private DateTime GetDateFromX(double x)
        {
            if (TimelineCanvas.ActualWidth <= 0) return _maxDate;
            double ratio = x / TimelineCanvas.ActualWidth;
            long ticksRange = (_maxDate - _minDate).Ticks;
            return _minDate.AddTicks((long)(ticksRange * ratio));
        }

        private void DrawDensityCurve()
        {
            GraphCanvas.Children.Clear();
            double width = GraphCanvas.ActualWidth;
            double height = GraphCanvas.ActualHeight;
            if (width <= 0) return;

            int segments = 40;
            var points = new PointCollection();
            long totalTicks = (_maxDate - _minDate).Ticks;
            if (totalTicks <= 0) return;

            for (int i = 0; i <= segments; i++)
            {
                double x = (width / segments) * i;
                DateTime segStart = _minDate.AddTicks((totalTicks / segments) * i);
                DateTime segEnd = (i == segments) ? _maxDate : _minDate.AddTicks((totalTicks / segments) * (i + 1));

                int count = _allClips.Count(c => c.CreationDate >= segStart && c.CreationDate < segEnd);
                double normalizedCount = Math.Min(count, 4); 
                double y = height - (normalizedCount * (height / 4.5)) - 10;
                points.Add(new Point(x, y));
            }

            var polyline = new Polyline
            {
                Points = points,
                Stroke = (Brush)Application.Current.Resources["SystemControlHighlightAccentBrush"],
                StrokeThickness = 4,
                Opacity = 0.9,
                StrokeLineJoin = PenLineJoin.Round
            };

            GraphCanvas.Children.Add(polyline);
        }

        private void DisplayClips(IEnumerable<ClipModel> clipsToDisplay)
        {
            if (clipsToDisplay == null) return;
            
            _displayedCards.Clear();
            _selectedIndex = -1;

            Column0.Children.Clear();
            Column1.Children.Clear();
            Column2.Children.Clear();

            var template = (DataTemplate)Resources["ClipCardTemplate"];
            int i = 0;
            foreach (var clip in clipsToDisplay)
            {
                var card = (FrameworkElement)template.LoadContent();
                card.DataContext = clip;
                _displayedCards.Add(card);

                int col = i % 3;
                if (col == 0) Column0.Children.Add(card);
                else if (col == 1) Column1.Children.Add(card);
                else Column2.Children.Add(card);
                i++;
            }
        }

        // --- KEYBOARD NAVIGATION ---
        private void Page_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (_displayedCards.Count == 0) return;
            
            // Ignorer si on est dans la barre de recherche
            if (FocusManager.GetFocusedElement(this.XamlRoot) is AutoSuggestBox || 
                FocusManager.GetFocusedElement(this.XamlRoot) is TextBox) return;

            bool isNavKey = true;
            int newIndex = _selectedIndex;

            switch (e.Key)
            {
                case Windows.System.VirtualKey.Right:
                    if (_selectedIndex < 0) newIndex = 0;
                    else newIndex = Math.Min(_displayedCards.Count - 1, _selectedIndex + 1);
                    break;
                case Windows.System.VirtualKey.Left:
                    if (_selectedIndex < 0) newIndex = 0;
                    else newIndex = Math.Max(0, _selectedIndex - 1);
                    break;
                case Windows.System.VirtualKey.Down:
                    if (_selectedIndex < 0) newIndex = 0;
                    else newIndex = Math.Min(_displayedCards.Count - 1, _selectedIndex + 3);
                    break;
                case Windows.System.VirtualKey.Up:
                    if (_selectedIndex < 0) newIndex = 0;
                    else newIndex = Math.Max(0, _selectedIndex - 3);
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
                e.Handled = true; // Empêche le ScrollViewer de scroller par défaut
                if (newIndex != _selectedIndex)
                {
                    UpdateSelection(newIndex);
                }
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

        private void SetSelectionVisual(FrameworkElement card, bool isSelected)
        {
            if (card.FindName("SelectionBorder") is Border border)
                border.Opacity = isSelected ? 1 : 0;
        }

        // --- TIMELINE LOGIC ---
        private void DateFilterButton_Click(object sender, RoutedEventArgs e)
        {
            _isTimelineVisible = !_isTimelineVisible;
            if (_isTimelineVisible) {
                ShowTimelineStoryboard.Begin();
                InitializeTimeline();
            }
            else HideTimelineStoryboard.Begin();
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
            double startX = Canvas.GetLeft(StartHandle) + 20;
            double endX = Canvas.GetLeft(EndHandle) + 20;
            double vStart = Math.Min(startX, endX);
            double vEnd = Math.Max(startX, endX);

            DateTime dStart = GetDateFromX(vStart);
            DateTime dEnd = GetDateFromX(vEnd);

            var filtered = _allClips.AsEnumerable();
            filtered = filtered.Where(c => c.CreationDate >= dStart && c.CreationDate <= dEnd);

            string query = SearchBox.Text.Trim().ToLower();
            if (!string.IsNullOrWhiteSpace(query))
            {
                var terms = query.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                filtered = filtered.Where(c => terms.All(t => c.Title.ToLower().Contains(t) || c.Game.ToLower().Contains(t)));
            }

            DisplayClips(filtered.ToList());
        }

        private void SearchBox_TextChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args)
        {
            var suggestions = _availableTags.Where(t => t.Contains(sender.Text, StringComparison.OrdinalIgnoreCase)).ToList();
            sender.ItemsSource = suggestions;
            FilterClips();
        }

        private void SearchBox_QuerySubmitted(AutoSuggestBox sender, AutoSuggestBoxQuerySubmittedEventArgs args) => FilterClips();

        // --- POINTER EVENTS ---
        private void CardRoot_PointerEntered(object sender, PointerRoutedEventArgs e)
        {
            if (sender is FrameworkElement card)
            {
                int hoverIndex = _displayedCards.IndexOf(card);
                if (hoverIndex != -1) UpdateSelection(hoverIndex);

                if (card.FindName("HoverPlayer") is MediaPlayerElement player)
                {
                    player.Opacity = 1;
                    player.MediaPlayer.IsLoopingEnabled = true;
                    player.MediaPlayer.Play();
                }
            }
        }

        private void CardRoot_PointerExited(object sender, PointerRoutedEventArgs e)
        {
            if (sender is FrameworkElement card && card.FindName("HoverPlayer") is MediaPlayerElement player)
            {
                player.Opacity = 0;
                player.MediaPlayer.Pause();
                player.MediaPlayer.PlaybackSession.Position = TimeSpan.Zero;
            }
        }

        private void CardRoot_Tapped(object sender, TappedRoutedEventArgs e)
        {
            if (sender is FrameworkElement card && card.DataContext is ClipModel clip)
            {
                Frame.Navigate(typeof(StudioPage), clip);
            }
        }
    }
}
