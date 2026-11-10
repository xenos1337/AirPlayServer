using System;
using System.Windows;
using System.Windows.Media;

namespace AirPlayServer;

// A line over one of the receiver's 30-second histories.
internal sealed class Sparkline : FrameworkElement
{
    public static readonly DependencyProperty StrokeProperty = DependencyProperty.Register(
        nameof(Stroke), typeof(Brush), typeof(Sparkline),
        new FrameworkPropertyMetadata(Brushes.Gray, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty GuideStrokeProperty = DependencyProperty.Register(
        nameof(GuideStroke), typeof(Brush), typeof(Sparkline),
        new FrameworkPropertyMetadata(Brushes.Gray, FrameworkPropertyMetadataOptions.AffectsRender));

    private float[] values = [];
    private int oldestIndex;

    public Brush Stroke
    {
        get => (Brush)GetValue(StrokeProperty);
        set => SetValue(StrokeProperty, value);
    }

    public Brush GuideStroke
    {
        get => (Brush)GetValue(GuideStrokeProperty);
        set => SetValue(GuideStrokeProperty, value);
    }

    public double Maximum { get; set; } = 1;

    // Draws a reference line at this value; NaN for none.
    public double Guide { get; set; } = double.NaN;

    public void SetValues(float[] history, int oldest)
    {
        values = history;
        oldestIndex = oldest;
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        double width = ActualWidth;
        double height = ActualHeight;

        if (!double.IsNaN(Guide) && Guide >= 0 && Guide <= Maximum)
        {
            double y = height - Guide / Maximum * height;
            Pen guidePen = new(GuideStroke, 1) { DashStyle = DashStyles.Dash };
            drawingContext.DrawLine(guidePen, new Point(0, y), new Point(width, y));
        }

        int count = values.Length;
        if (count < 2)
        {
            return;
        }
        StreamGeometry geometry = new();
        using (StreamGeometryContext context = geometry.Open())
        {
            double step = width / (count - 1);
            for (int i = 0; i < count; i++)
            {
                double value = Math.Clamp(values[(oldestIndex + i) % count] / Maximum, 0, 1);
                Point point = new(step * i, height - value * height);
                if (i == 0)
                {
                    context.BeginFigure(point, false, false);
                }
                else
                {
                    context.LineTo(point, true, true);
                }
            }
        }
        drawingContext.DrawGeometry(null, new Pen(Stroke, 1.5) { LineJoin = PenLineJoin.Round }, geometry);
    }
}
