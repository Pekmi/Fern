using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using System;
using System.Text;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading.Tasks;
using Windows.Foundation;
using Windows.Foundation.Collections;

namespace FernUI
{
    public sealed partial class MainWindow : Window
    {
        private Microsoft.UI.Windowing.AppWindow _appWindow;
        private readonly DispatcherTimer _daemonStatusTimer = new();
        private bool _isCheckingDaemonStatus;
        private bool? _lastDaemonActive;
        private bool _isContentFullScreen;
        private bool _wasWindowFullScreenBeforeContentFullScreen;

        public MainWindow()
        {
            this.InitializeComponent();

            // Configurer la taille au démarrage
            IntPtr hWnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
            Microsoft.UI.WindowId windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(hWnd);
            _appWindow = Microsoft.UI.Windowing.AppWindow.GetFromWindowId(windowId);
            _appWindow.Resize(new Windows.Graphics.SizeInt32(1280, 720));

            ExtendsContentIntoTitleBar = true;
            SetTitleBar(null);

            _daemonStatusTimer.Interval = TimeSpan.FromSeconds(2);
            _daemonStatusTimer.Tick += DaemonStatusTimer_Tick;
            _daemonStatusTimer.Start();
            _ = RefreshDaemonStatusAsync();
        }

        private async void CaptureButton_Click(object sender, RoutedEventArgs e)
        {
            await SendSaveCommandAsync();
        }

        public async Task SendSaveCommandAsync()
        {
            try
            {
                using (var pipeClient = new NamedPipeClientStream(".", "FernPipe", PipeDirection.Out))
                {
                    await pipeClient.ConnectAsync(1000);

                    using (var writer = new StreamWriter(pipeClient))
                    {
                        writer.AutoFlush = true;
                        await writer.WriteAsync("SAVE");
                    }
                }

                UpdateDaemonStatus(true);
            }
            catch (Exception)
            {
                UpdateDaemonStatus(false);
            }
        }

        private async void DaemonStatusTimer_Tick(object? sender, object e)
        {
            await RefreshDaemonStatusAsync();
        }

        private async Task RefreshDaemonStatusAsync()
        {
            if (_isCheckingDaemonStatus) return;

            _isCheckingDaemonStatus = true;
            try
            {
                bool isActive = await IsDaemonAvailableAsync();
                UpdateDaemonStatus(isActive);
            }
            finally
            {
                _isCheckingDaemonStatus = false;
            }
        }

        private static async Task<bool> IsDaemonAvailableAsync()
        {
            try
            {
                using var pipeClient = new NamedPipeClientStream(".", "FernPipe", PipeDirection.Out, PipeOptions.Asynchronous);
                await pipeClient.ConnectAsync(200);
                return pipeClient.IsConnected;
            }
            catch (Exception)
            {
                return false;
            }
        }

        private void UpdateDaemonStatus(bool isActive)
        {
            if (_lastDaemonActive == isActive) return;

            _lastDaemonActive = isActive;
            string label = isActive ? "Daemon actif" : "Daemon inactif";
            DaemonStatusItem.Content = label;
            ToolTipService.SetToolTip(DaemonStatusItem, label);
            DaemonStatusIcon.Foreground = new SolidColorBrush(isActive
                ? Windows.UI.Color.FromArgb(255, 62, 203, 127)
                : Windows.UI.Color.FromArgb(255, 229, 72, 77));
        }

        private void FullScreenButton_Click(object sender, RoutedEventArgs e)
        {
            ToggleFullScreen();
        }

        private void ToggleFullScreen()
        {
            if (_appWindow.Presenter.Kind == Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen)
            {
                _appWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.Default);
            }
            else
            {
                _appWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen);
            }
        }

        public void SetContentFullScreen(bool isFullScreen)
        {
            if (_isContentFullScreen == isFullScreen) return;

            _isContentFullScreen = isFullScreen;

            if (isFullScreen)
            {
                _wasWindowFullScreenBeforeContentFullScreen = _appWindow.Presenter.Kind == Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen;
                if (!_wasWindowFullScreenBeforeContentFullScreen)
                {
                    _appWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen);
                }

                AppChromeBar.Visibility = Visibility.Collapsed;
                NavView.Visibility = Visibility.Collapsed;
                Grid.SetRow(ContentFrame, 0);
                Grid.SetColumn(ContentFrame, 0);
                Grid.SetRowSpan(ContentFrame, 2);
                Grid.SetColumnSpan(ContentFrame, 2);
                Canvas.SetZIndex(ContentFrame, 10);
            }
            else
            {
                if (!_wasWindowFullScreenBeforeContentFullScreen)
                {
                    _appWindow.SetPresenter(Microsoft.UI.Windowing.AppWindowPresenterKind.Default);
                }

                AppChromeBar.Visibility = Visibility.Visible;
                NavView.Visibility = Visibility.Visible;
                Grid.SetRow(ContentFrame, 1);
                Grid.SetColumn(ContentFrame, 1);
                Grid.SetRowSpan(ContentFrame, 1);
                Grid.SetColumnSpan(ContentFrame, 1);
                Canvas.SetZIndex(ContentFrame, 0);
            }
        }

        private void KeyboardAccelerator_Invoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
        {
            if (sender.Key == Windows.System.VirtualKey.F11)
            {
                ToggleFullScreen();
                args.Handled = true;
            }
        }

        private void NavView_Loaded(object sender, RoutedEventArgs e)
        {
            NavView.SelectedItem = NavView.MenuItems[0];
            NavigateTo(typeof(Views.GalleryPage), "Ma Galerie");
        }

        private void NavView_ItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
        {
            if (args.IsSettingsInvoked)
            {
                NavigateTo(typeof(Views.SettingsPage), "Paramètres");
            }
            else if (args.InvokedItemContainer != null)
            {
                var tag = args.InvokedItemContainer.Tag as string;
                switch (tag)
                {
                    case "GalleryPage":
                        NavigateTo(typeof(Views.GalleryPage), "Ma Galerie");
                        break;
                    case "CloudSharesPage":
                        NavigateTo(typeof(Views.CloudSharesPage), "Partages Cloud");
                        break;
                }
            }
        }

        private void NavigateTo(Type pageType, string title)
        {
            PageTitleText.Text = title;
            ContentFrame.Navigate(pageType);
        }
    }
}
