using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using FernUI.Models;

namespace FernUI.Views
{
    public sealed partial class CloudSharesPage : Page
    {
        public ObservableCollection<CloudClip> Shares { get; } = new();

        public CloudSharesPage()
        {
            this.InitializeComponent();
            CloudSharesListView.ItemsSource = Shares;
            Loaded += CloudSharesPage_Loaded;
        }

        private async void CloudSharesPage_Loaded(object sender, RoutedEventArgs e)
        {
            await RefreshCloudClipsAsync();
        }

        private async Task RefreshCloudClipsAsync()
        {
            try
            {
                var clips = await CloudService.GetCloudClipsAsync();
                Shares.Clear();
                foreach (var clip in clips)
                {
                    Shares.Add(clip);
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to refresh cloud clips: {ex.Message}");
            }
        }

        private void CopyLinkButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button btn && btn.DataContext is CloudClip item)
            {
                var package = new Windows.ApplicationModel.DataTransfer.DataPackage();
                package.SetText(item.Link);
                Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(package);
                _ = ShowCopyFeedbackAsync(btn);
            }
        }

        private async Task ShowCopyFeedbackAsync(Button button)
        {
            object originalContent = button.Content;
            button.Content = CreateActionContent("\uE73E", "Copié");

            if (button.RenderTransform is not CompositeTransform transform)
            {
                transform = new CompositeTransform();
                button.RenderTransform = transform;
                button.RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5);
            }

            transform.ScaleX = 0.94;
            transform.ScaleY = 0.94;

            var storyboard = new Storyboard();
            storyboard.Children.Add(CreateScaleAnimation(transform, nameof(CompositeTransform.ScaleX)));
            storyboard.Children.Add(CreateScaleAnimation(transform, nameof(CompositeTransform.ScaleY)));
            storyboard.Begin();

            await Task.Delay(950);

            if (button.XamlRoot != null)
            {
                button.Content = originalContent;
            }
        }

        private static StackPanel CreateActionContent(string glyph, string text)
        {
            return new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Children =
                {
                    new FontIcon { Glyph = glyph, FontSize = 14, Margin = new Thickness(0, 0, 8, 0) },
                    new TextBlock { Text = text }
                }
            };
        }

        private static DoubleAnimation CreateScaleAnimation(DependencyObject target, string property)
        {
            var animation = new DoubleAnimation
            {
                To = 1,
                Duration = TimeSpan.FromMilliseconds(180),
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
            };

            Storyboard.SetTarget(animation, target);
            Storyboard.SetTargetProperty(animation, property);
            return animation;
        }

        private async void DeleteClipButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is not Button btn || btn.DataContext is not CloudClip item)
            {
                return;
            }

            if (btn.Tag is not string state || state != "ConfirmDelete")
            {
                ArmDeleteConfirmation(btn);
                return;
            }

            btn.IsEnabled = false;
            try
            {
                bool deleted = await CloudService.DeleteCloudClipAsync(item.Id);
                if (deleted)
                {
                    Shares.Remove(item);
                    return;
                }

                await ShowErrorAsync("Suppression impossible", "Le serveur n'a pas supprimé ce clip.");
                ResetDeleteButton(btn);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to delete cloud clip: {ex.Message}");
                await ShowErrorAsync("Suppression impossible", ex.Message);
                ResetDeleteButton(btn);
            }
            finally
            {
                btn.IsEnabled = true;
            }
        }

        private static void ArmDeleteConfirmation(Button button)
        {
            button.Tag = "ConfirmDelete";
            button.Content = CreateActionContent("\uE74D", "Confirmer");
            var foregroundBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 255, 255));
            var backgroundBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 196, 43, 43));
            var hoverBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 214, 48, 48));
            var pressedBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 154, 30, 30));

            button.Foreground = foregroundBrush;
            button.Background = backgroundBrush;
            button.Resources["ButtonForegroundPointerOver"] = foregroundBrush;
            button.Resources["ButtonForegroundPressed"] = foregroundBrush;
            button.Resources["ButtonBackgroundPointerOver"] = hoverBrush;
            button.Resources["ButtonBackgroundPressed"] = pressedBrush;
            button.Resources["ButtonBorderBrushPointerOver"] = hoverBrush;
            button.Resources["ButtonBorderBrushPressed"] = pressedBrush;

            if (button.RenderTransform is not CompositeTransform transform)
            {
                transform = new CompositeTransform();
                button.RenderTransform = transform;
                button.RenderTransformOrigin = new Windows.Foundation.Point(0.5, 0.5);
            }

            transform.ScaleX = 0.94;
            transform.ScaleY = 0.94;

            var storyboard = new Storyboard();
            storyboard.Children.Add(CreateConfirmPopAnimation(transform, nameof(CompositeTransform.ScaleX)));
            storyboard.Children.Add(CreateConfirmPopAnimation(transform, nameof(CompositeTransform.ScaleY)));
            storyboard.Begin();
        }

        private static void ResetDeleteButton(Button button)
        {
            button.Tag = null;
            button.Content = CreateActionContent("\uE74D", "Supprimer");
            button.ClearValue(Control.ForegroundProperty);
            button.ClearValue(Control.BackgroundProperty);
            button.Resources.Remove("ButtonForegroundPointerOver");
            button.Resources.Remove("ButtonForegroundPressed");
            button.Resources.Remove("ButtonBackgroundPointerOver");
            button.Resources.Remove("ButtonBackgroundPressed");
            button.Resources.Remove("ButtonBorderBrushPointerOver");
            button.Resources.Remove("ButtonBorderBrushPressed");
        }

        private static DoubleAnimationUsingKeyFrames CreateConfirmPopAnimation(DependencyObject target, string property)
        {
            var animation = new DoubleAnimationUsingKeyFrames();
            animation.KeyFrames.Add(new EasingDoubleKeyFrame
            {
                KeyTime = KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(0)),
                Value = 0.90
            });
            animation.KeyFrames.Add(new EasingDoubleKeyFrame
            {
                KeyTime = KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(170)),
                Value = 1.10,
                EasingFunction = new BackEase
                {
                    EasingMode = EasingMode.EaseOut,
                    Amplitude = 0.32
                }
            });
            animation.KeyFrames.Add(new EasingDoubleKeyFrame
            {
                KeyTime = KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(360)),
                Value = 1.0,
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
            });

            Storyboard.SetTarget(animation, target);
            Storyboard.SetTargetProperty(animation, property);
            return animation;
        }

        private async Task ShowErrorAsync(string title, string message)
        {
            var dialog = new ContentDialog
            {
                XamlRoot = XamlRoot,
                Title = title,
                Content = message,
                CloseButtonText = "OK"
            };

            await dialog.ShowAsync();
        }
    }
}
