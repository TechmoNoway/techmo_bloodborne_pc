using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;

namespace BloodborneLauncher;

internal sealed record OnlineProfile(string Host, int Port, string WebApi, string GameApi, string Website)
{
    internal string Server => Host.Contains(':') ? $"[{Host}]:{Port}" : $"{Host}:{Port}";
    internal static string HttpBase(string text, string field)
    {
        text = text.Trim();
        if (!Uri.TryCreate(text, UriKind.Absolute, out var url) ||
            url.Scheme is not ("http" or "https") || string.IsNullOrEmpty(url.Host) ||
            !string.IsNullOrEmpty(url.UserInfo) || !string.IsNullOrEmpty(url.Query) ||
            !string.IsNullOrEmpty(url.Fragment) || text.Any(char.IsControl))
            throw new IOException(L10n.Text("{0}: enter a valid HTTP or HTTPS URL, including the port if needed.", L10n.Text(field)));
        return text.TrimEnd('/');
    }
    internal static OnlineProfile Create(string host, string port, string api, string game, string website)
    {
        host = host.Trim().Trim('[', ']');
        if (Uri.CheckHostName(host) == UriHostNameType.Unknown || host.Any(char.IsWhiteSpace))
            throw new IOException(L10n.Text("Enter your shadNet server's IP or hostname."));
        if (host.Contains(':')) throw new IOException(L10n.Text("This version of shadNet requires an IPv4 address or hostname."));
        if (!int.TryParse(port, out int tcp) || tcp is < 1 or > 65535)
            throw new IOException(L10n.Text("The TCP port must be between 1 and 65535."));
        string web = HttpBase(api, "WebAPI");
        string gameApi = string.IsNullOrWhiteSpace(game) ? web : HttpBase(game, "Game API");
        if (new Uri(web).AbsolutePath != "/" || new Uri(gameApi).AbsolutePath != "/")
            throw new IOException(L10n.Text("WebAPI and game API must specify the server and port without an additional path."));
        return new(host, tcp, web, gameApi,
                   string.IsNullOrWhiteSpace(website) ? "" : HttpBase(website, "Account website"));
    }

    internal static string P2pPort(string text)
    {
        text=text.Trim();
        if (text.Length==0) return "";
        if (!int.TryParse(text,out int port) || port is <1 or >65535)
            throw new IOException(L10n.Text("The P2P port must be between 1 and 65535, or empty for automatic selection."));
        return port.ToString(System.Globalization.CultureInfo.InvariantCulture);
    }

    // Reachability and bootstrap checks do not create accounts or perform a login.
    internal async Task<string> ProbeAsync()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(7));
        using var socket = new TcpClient();
        await socket.ConnectAsync(Host, Port, timeout.Token);
        byte[] greeting = new byte[19];
        int offset = 0;
        while (offset < greeting.Length)
        {
            int n = await socket.GetStream().ReadAsync(greeting.AsMemory(offset), timeout.Token);
            if (n == 0) throw new IOException(L10n.Text("shadNet closed the connection during the handshake."));
            offset += n;
        }
        if (greeting[0] != 3 || BitConverter.ToUInt32(greeting, 3) != 19 ||
            BitConverter.ToUInt32(greeting, 15) != 1)
            throw new IOException(L10n.Text("The server does not advertise a compatible shadNet v1 protocol."));
        using var http = new System.Net.Http.HttpClient { Timeout = TimeSpan.FromSeconds(7) };
        using var response = await http.GetAsync(GameApi + "/bb-eu/ss.info", System.Net.Http.HttpCompletionOption.ResponseHeadersRead, timeout.Token);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength > 65536) throw new IOException(L10n.Text("ss.info exceeds the allowed size."));
        using var body = await response.Content.ReadAsStreamAsync(timeout.Token);
        using var data = new MemoryStream();
        byte[] block = new byte[4096];
        int count;
        while ((count = await body.ReadAsync(block, timeout.Token)) != 0)
        {
            if (data.Length + count > 65536) throw new IOException(L10n.Text("ss.info exceeds the allowed size."));
            data.Write(block, 0, count);
        }
        string xml = Encoding.UTF8.GetString(Convert.FromBase64String(Encoding.ASCII.GetString(data.ToArray())));
        var apis = Regex.Matches(xml, @"<api_[^>]+>([^<]+)</api_[^>]+>");
        if (apis.Count < 30) throw new IOException(L10n.Text("The server responds, but the Bloodborne 1.09 bootstrap is missing."));
        foreach (Match item in apis)
        {
            var target = new Uri(HttpBase(item.Groups[1].Value, "ss.info route"));
            var selected = new Uri(GameApi);
            if (target.Scheme != selected.Scheme || target.Host != selected.Host || target.Port != selected.Port)
                throw new IOException(L10n.Text("ss.info routes an API to another server. Check PublicBaseUrl in shadNet."));
        }
        return L10n.Text("shadNet v1 connected · valid game bootstrap ({0} routes). Login and multiplayer require in-game testing.", apis.Count);
    }
}
