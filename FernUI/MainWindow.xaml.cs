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

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace FernUI
{
    /// <summary>
    /// An empty window that can be used on its own or navigated to within a Frame.
    /// </summary>
    public sealed partial class MainWindow : Window
    {
        private Microsoft.UI.Windowing.AppWindow _appWindow;
        private readonly DispatcherTimer _daemonStatusTimer = new();
        private bool _isCheckingDaemonStatus;
        private bool? _lastDaemonActive;

        public MainWindow()
        {
            InitializeComponent();
            ExtendsContentIntoTitleBar = true;
            SetTitleBar(null);

            IntPtr hWnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
            var windowId = Microsoft.UI.Win32Interop.GetWindowIdFromWindow(hWnd);
            _appWindow = Microsoft.UI.Windowing.AppWindow.GetFromWindowId(windowId);
            _appWindow.Resize(new Windows.Graphics.SizeInt32 { Width = 1400, Height = 800 });

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
                // Silently fail for now, or log if needed
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
            catch (TimeoutException)
            {
                return false;
            }
            catch (IOException)
            {
                return false;
            }
            catch (UnauthorizedAccessException)
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
            ContentFrame.Navigate(typeof(Views.GalleryPage));
        }

        private void NavView_ItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
        {
            if (args.IsSettingsInvoked)
            {
                ContentFrame.Navigate(typeof(Views.SettingsPage));
            }
            else if (args.InvokedItemContainer != null)
            {
                var tag = args.InvokedItemContainer.Tag as string;
                switch (tag)
                {
                    case "GalleryPage":
                        ContentFrame.Navigate(typeof(Views.GalleryPage));
                        break;
                    case "CloudSharesPage":
                        ContentFrame.Navigate(typeof(Views.CloudSharesPage));
                        break;
                }
            }
        }
    }
}
