using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Collections.ObjectModel;
using FernUI.Models;

namespace FernUI.Views
{
    public sealed partial class CloudSharesPage : Page
    {
        public ObservableCollection<CloudShareItem> Shares { get; set; }

        public CloudSharesPage()
        {
            this.InitializeComponent();

            Shares = new ObservableCollection<CloudShareItem>
            {
                new CloudShareItem { Title = "Quad Feed - Valorant", Game = "VALORANT", Link = "fern.live/c/x9f2a", Expiration = "Expire dans 4 jours" },
                new CloudShareItem { Title = "Fail Hilarious", Game = "GTAV", Link = "fern.live/c/b3k7m", Expiration = "Expire dans 16 heures" },
                new CloudShareItem { Title = "World Record attempt", Game = "ELDEN RING", Link = "fern.live/c/z8r1p", Expiration = "Expire dans 6 jours" }
            };

            CloudSharesListView.ItemsSource = Shares;
        }

        private void CopyLinkButton_Click(object sender, RoutedEventArgs e)
        {
            // Simulation de copie dans le presse-papiers
            if (sender is Button btn && btn.DataContext is CloudShareItem item)
            {
                var package = new Windows.ApplicationModel.DataTransfer.DataPackage();
                package.SetText(item.Link);
                Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(package);
            }
        }
    }
}
