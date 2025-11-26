# airplay2-win
Airplay2 for windows.

Migrate [AirplayServer](https://github.com/KqSMea8/AirplayServer) and [dnssd](https://github.com/jevinskie/mDNSResponder) to Windows Platform.

## Build

- Open `airplay2-win.sln` in Visual Studio 2019.
- Make `AirPlayServer` as Start Project.
- `Ctrl + B`, Build `AirPlayServer`.
- The generated exe will be placed in `x64\Debug` folder.

## Reference

- [shairplay](https://github.com/juhovh/shairplay) 
- [dnssd](https://github.com/jevinskie/mDNSResponder)
- [AirplayServer](https://github.com/KqSMea8/AirplayServer)
- [xindawn-windows-airplay-mirroring-sdk](https://github.com/xindawndev/xindawn-windows-airplay-mirroring-sdk)


## FAQ

- Can be discovered by iOS devices, but cannot connect.

  Make sure iOS and Windows are on the same Wi-Fi network and the same subnet. If Windows is running in a virtual machine, make sure you are using bridged networking instead of shared networking.
