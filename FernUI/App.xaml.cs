using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using Microsoft.UI.Xaml.Shapes;
using FernUI.Models;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text;
using System.Threading.Tasks;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Activation;
using Windows.Foundation;
using Windows.Foundation.Collections;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace FernUI
{
    /// <summary>
    /// Provides application-specific behavior to supplement the default Application class.
    /// </summary>
    public partial class App : Application
    {
        private Window? _window;
        public static Window? MainWindow { get; private set; }

        /// <summary>
        /// Initializes the singleton application object.  This is the first line of authored code
        /// executed, and as such is the logical equivalent of main() or WinMain().
        /// </summary>
        public App()
        {
            UnhandledException += App_UnhandledException;
            AppDomain.CurrentDomain.UnhandledException += CurrentDomain_UnhandledException;
            TaskScheduler.UnobservedTaskException += TaskScheduler_UnobservedTaskException;

            try
            {
                InitializeComponent();
            }
            catch (Exception ex)
            {
                ReportFatalError("FernUI failed during XAML initialization.", ex);
                throw;
            }
        }

        /// <summary>
        /// Invoked when the application is launched.
        /// </summary>
        /// <param name="args">Details about the launch request and process.</param>
        protected override void OnLaunched(Microsoft.UI.Xaml.LaunchActivatedEventArgs args)
        {
            try
            {
                ClipLibraryService.DeleteFernAudioTempFiles();

                _window = new MainWindow();
                MainWindow = _window;
                _window.Activate();
            }
            catch (Exception ex)
            {
                ReportFatalError("FernUI failed during launch.", ex);
                throw;
            }
        }

        private static void App_UnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs e)
        {
            ReportFatalError("FernUI crashed.", e.Exception);
        }

        private static void CurrentDomain_UnhandledException(object sender, System.UnhandledExceptionEventArgs e)
        {
            if (e.ExceptionObject is Exception ex)
            {
                ReportFatalError("FernUI crashed outside the UI dispatcher.", ex);
            }
            else
            {
                ReportFatalError("FernUI crashed outside the UI dispatcher.", null, e.ExceptionObject?.ToString());
            }
        }

        private static void TaskScheduler_UnobservedTaskException(object? sender, UnobservedTaskExceptionEventArgs e)
        {
            ReportFatalError("FernUI encountered an unobserved task exception.", e.Exception);
            e.SetObserved();
        }

        private static void ReportFatalError(string message, Exception? exception, string? details = null)
        {
            string logPath = WriteCrashLog(message, exception, details);
            string body = $"{message}\n\n{exception?.Message ?? details ?? "No exception details available."}\n\nLog: {logPath}";
            Debug.WriteLine(body);
            MessageBoxW(IntPtr.Zero, body, "FernUI error", 0x00000010);
        }

        private static string WriteCrashLog(string message, Exception? exception, string? details)
        {
            string appData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            string logDirectory = System.IO.Path.Combine(appData, "PekmisIndustries", "Fern", "logs");
            Directory.CreateDirectory(logDirectory);

            string logPath = System.IO.Path.Combine(logDirectory, "FernUI-crash.log");
            var builder = new StringBuilder();
            builder.AppendLine("==== FernUI crash ====");
            builder.AppendLine(DateTimeOffset.Now.ToString("O"));
            builder.AppendLine(message);
            builder.AppendLine(details ?? exception?.ToString() ?? "No exception details available.");
            builder.AppendLine();
            File.AppendAllText(logPath, builder.ToString());
            return logPath;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int MessageBoxW(IntPtr hWnd, string text, string caption, uint type);
    }
}
