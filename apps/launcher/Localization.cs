using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Markup;

namespace BloodborneLauncher;

// UI language is independent of the numeric language ID consumed by the game.
public sealed class Localization : INotifyPropertyChanged
{
    public static Localization Current { get; } = new();
    readonly Dictionary<string, string> english = ReadCatalog("en");
    readonly Dictionary<string, string> vietnamese = ReadCatalog("vi");
    readonly Dictionary<string, string> keysByText;
    public string Language { get; private set; } = "en";
    public event PropertyChangedEventHandler? PropertyChanged;

    Localization()
    {
        keysByText = english.ToDictionary(pair => pair.Value, pair => pair.Key, StringComparer.Ordinal);
        foreach (var pair in vietnamese) keysByText.TryAdd(pair.Value, pair.Key);
        if (!english.Keys.ToHashSet().SetEquals(vietnamese.Keys))
            throw new InvalidDataException("English and Vietnamese UI catalogs must contain the same keys.");
    }

    static Dictionary<string, string> ReadCatalog(string language)
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream($"BloodborneLauncher.Localization.{language}.json")
            ?? throw new InvalidDataException($"Missing UI language catalog: {language}");
        return JsonSerializer.Deserialize<Dictionary<string, string>>(stream)
            ?? throw new InvalidDataException($"Invalid UI language catalog: {language}");
    }

    public static string Normalize(string? language) => language == "vi" ? "vi" : "en";

    public void SetLanguage(string? language)
    {
        string normalized = Normalize(language);
        if (Language == normalized) return;
        Language = normalized;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Language)));
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs("Item[]"));
    }

    public string this[string key] => Language == "vi" && vietnamese.TryGetValue(key, out var translated)
        ? translated : english.GetValueOrDefault(key, key);

    public string Translate(string text, params object[] arguments)
    {
        string template = keysByText.TryGetValue(text, out var key) ? this[key] : text;
        return arguments.Length == 0 ? template : string.Format(CultureInfo.InvariantCulture, template, arguments);
    }
}

[MarkupExtensionReturnType(typeof(string))]
public sealed class LocExtension : MarkupExtension
{
    public string Key { get; set; } = "";
    public override object ProvideValue(IServiceProvider serviceProvider) =>
        new Binding($"[{Key}]") { Source = Localization.Current, Mode = BindingMode.OneWay }.ProvideValue(serviceProvider);
}

internal static class L10n
{
    internal static string Text(string text, params object[] arguments) => Localization.Current.Translate(text, arguments);

    // Bind transient statuses too, so a language switch also translates existing messages.
    internal static void SetText(TextBlock block, string text, params object[] arguments) =>
        BindingOperations.SetBinding(block, TextBlock.TextProperty, new Binding(nameof(Localization.Language))
        {
            Source = Localization.Current,
            Converter = new MessageConverter(text, arguments),
            Mode = BindingMode.OneWay
        });

    sealed class MessageConverter(string text, object[] arguments) : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture) =>
            Text(text, arguments.Select(argument => argument is string message ? Text(message) : argument).ToArray());
        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture) => Binding.DoNothing;
    }
}
