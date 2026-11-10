using System.Runtime.InteropServices;
using System.Windows.Interop;

namespace AirPlayServer;

// Hosts the video child window that AirPlayCore creates and renders into on
// its own thread. AirPlayCore also destroys it, in Native.Stop().
internal sealed class VideoHost : HwndHost
{
    private readonly nint view;

    public VideoHost(nint view)
    {
        this.view = view;
    }

    protected override HandleRef BuildWindowCore(HandleRef hwndParent)
    {
        return new HandleRef(this, view);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
    }
}
