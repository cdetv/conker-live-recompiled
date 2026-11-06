using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Documents;
using Avalonia.Layout;
using Avalonia.Media;
using ConkerLauncher.Services;

namespace ConkerLauncher.Views;

/// <summary>A release page's text (<see cref="ReleaseText"/>): headings, paragraphs and bullets, selectable.</summary>
public sealed class ReleaseNotesView : StackPanel
{
    public static readonly StyledProperty<string?> MarkdownProperty =
        AvaloniaProperty.Register<ReleaseNotesView, string?>(nameof(Markdown));

    static ReleaseNotesView()
    {
        MarkdownProperty.Changed.AddClassHandler<ReleaseNotesView>((view, _) => view.Rebuild());
    }

    public ReleaseNotesView()
    {
        Spacing = 6;
    }

    public string? Markdown
    {
        get => GetValue(MarkdownProperty);
        set => SetValue(MarkdownProperty, value);
    }

    private void Rebuild()
    {
        Children.Clear();
        List<TextBlockInfo> blocks = ReleaseText.Parse(Markdown ?? "");
        if (blocks.Count == 0)
        {
            Children.Add(new TextBlock { Text = "No notes for this release.", Foreground = Brushes.Gray });
            return;
        }

        foreach (TextBlockInfo block in blocks)
        {
            SelectableTextBlock text = new() { TextWrapping = TextWrapping.Wrap };
            foreach (TextSpan span in block.Spans)
            {
                text.Inlines!.Add(new Run(span.Text) { FontWeight = span.Bold || block.Heading ? FontWeight.SemiBold : FontWeight.Normal });
            }

            if (block.Heading)
            {
                text.FontSize = 16;
                text.Margin = new Thickness(0, 6, 0, 0);
            }

            if (block.Level == 0)
            {
                Children.Add(text);
                continue;
            }

            // A bullet: the dot, then its text wrapping beside it.
            Grid row = new() { ColumnDefinitions = new ColumnDefinitions("Auto,*"), Margin = new Thickness((block.Level - 1) * 18, 0, 0, 0) };
            row.Children.Add(new TextBlock { Text = block.Level == 1 ? "•" : "◦", Margin = new Thickness(0, 0, 8, 0), VerticalAlignment = VerticalAlignment.Top });
            Grid.SetColumn(text, 1);
            row.Children.Add(text);
            Children.Add(row);
        }
    }
}
