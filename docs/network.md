# Network and firewall

LSL finds streams with UDP broadcast and multicast, and transfers the data with TCP. Thus the viewer must accept inbound traffic, and the sources must be on the same subnet. This page tells you which ports LSL uses, what the Windows installer does, and how to correct a machine that cannot see the streams.

For the LSL side of the subject, see the [LSL network connectivity guide](https://labstreaminglayer.readthedocs.io/info/network-connectivity.html).

## Ports

| Port | Protocol | Use |
|---|---|---|
| 16571 | UDP | Service discovery. A resolver sends its query here as a broadcast, as a multicast, or as both, and each provider listens here and replies. |
| 16572 to 16604 | TCP | Stream data. |
| 16572 to 16604 | UDP | Service port for each outlet: clock synchronization, and replies about the stream. |
| 22345 | TCP | The viewer remote-control port, if you turn it on. See the [Remote control](../README.md#remote-control) section. |

The viewer is both a consumer and a provider: it reads the streams, and it announces its own control endpoint as an LSL stream. Thus it needs the inbound rules that a source needs, not only the rules that a recorder needs.

liblsl takes the ports from 16572 in pairs, one TCP port and one UDP port for each outlet. Thus a machine can hold 16 outlets in the default range. The range and the discovery port are `BasePort`, `PortRange`, and `MulticastPort` in [lsl_api.cfg](https://labstreaminglayer.readthedocs.io/info/lslapicfg.html), if your site changed them.

**IPv6 is on by default.** The `IPv6` setting in `lsl_api.cfg` is `allow`, thus liblsl uses IPv4 and IPv6 side by side, and it sends the discovery query to IPv4 and IPv6 multicast groups. A rule that permits IPv4 only can make the resolve fail, or make it slow while it waits for the IPv6 attempt. Permit both families, or set `IPv6 = disable` in the configuration file.

Where the firewall can match on the program (Windows and macOS), permit the program and not the ports. liblsl selects a port from the range at run time, and the control listener can move to a port that the operating system gives it, thus a rule for one port is not sufficient. `firewalld` and `ufw` match on ports only, so the Linux instructions below open the range.

## Windows

### What the installer does

The installer has a **Windows Firewall** task, which is selected by default. It adds one inbound allow rule for `lsl_viewer.exe` and one for `xdf_record.exe`, for the private and domain profiles. A second, cleared checkbox adds the public profile too. The uninstaller deletes the rules.

The task exists because the alternative is worse. With no rule, the first launch raises the "Windows Defender Firewall has blocked some features" prompt. A user who selects **Cancel**, or who cannot supply administrator credentials, gets a block rule that stays on the machine. The viewer then starts correctly but finds no streams, and nothing tells you why.

For an unattended install, use `/TASKS=""` to clear the task:

```
lsl-viewer-setup.exe /VERYSILENT /TASKS=""
```

### Add the rules by hand

Use this for the portable build, or after an install with the task cleared. Run in an administrator terminal:

```powershell
netsh advfirewall firewall add rule name="LSL Viewer" dir=in action=allow enable=yes profile=private,domain program="C:\Program Files\LSL Viewer\lsl_viewer.exe"
```

Give the full path to the executable that you run. A rule points to one file, thus a portable copy in a different directory needs its own rule.

### Troubleshooting

**The viewer finds no streams, but the source machine can see its own.**

1. Look for a block rule. A block rule wins over an allow rule, whatever the order.

   ```powershell
   netsh advfirewall firewall show rule name=all dir=in | Select-String -Context 3,9 lsl_viewer
   ```

   If the output has `Action: Block`, delete those rules and add the allow rule again:

   ```powershell
   netsh advfirewall firewall delete rule name="lsl_viewer.exe"
   ```

   Windows names the rules that its prompt creates after the program file, thus the name is not always the name that the installer uses. Delete by program instead when you are not sure of the name:

   ```powershell
   netsh advfirewall firewall delete rule name=all program="C:\Program Files\LSL Viewer\lsl_viewer.exe"
   ```

2. Check the network profile. A rule for the private and domain profiles does nothing on a network that Windows classifies as public. This is a frequent cause, because Windows gives the public class to an isolated lab network that has no gateway and no domain.

   ```powershell
   Get-NetConnectionProfile
   ```

   Either set the network to private, which is correct for a lab subnet that you control, or add the public profile to the rule:

   ```powershell
   Set-NetConnectionProfile -InterfaceAlias "Ethernet" -NetworkCategory Private
   ```

3. Check the order of the network adapters, if the machine has more than one. This is frequent in a lab, where an amplifier has a dedicated adapter and the internet comes through Wi-Fi. Windows sends the discovery query through the adapter with the lowest metric, which is not always the adapter on the lab network.

   ```powershell
   Get-NetIPInterface | Sort-Object InterfaceMetric | Format-Table InterfaceAlias, AddressFamily, InterfaceMetric
   ```

   Give the lab adapter a lower metric than the others:

   ```powershell
   Set-NetIPInterface -InterfaceAlias "Ethernet" -InterfaceMetric 10
   ```

4. Check that the machines are on the same subnet. Multicast does not cross a router unless the network is configured for it. If they are on different subnets, set `KnownPeers` in the [LSL configuration file](https://labstreaminglayer.readthedocs.io/info/lslapicfg.html) to list the source machines by address. `KnownPeers` replaces the multicast discovery with a direct connection to each address that you give.

**Only some streams appear.** The resolve traffic and the data traffic use different ports. If the streams show in the list but do not connect, the rule permits UDP but not TCP. A program rule with no protocol covers both; a port rule does not.

## macOS

The macOS application firewall is not the usual cause of trouble. It is off by default, and it only filters inbound traffic. The **Local Network** privacy control is the usual cause.

### Local network permission

macOS 15 (Sequoia) and later hold back the local network until you permit it, for each application. LSL resolves the streams with UDP broadcast and multicast, thus the viewer must have this permission. Without it, the viewer starts correctly and finds no streams, and macOS gives no error.

The first launch raises a prompt. Select **Allow**. To see or change the setting afterwards, go to **System Settings > Privacy & Security > Local Network**.

Two things make this more difficult than it looks:

- **The `.app` is unsigned.** macOS holds the permission against the code signature of the application. The release build has only the ad-hoc signature that the linker applies, and that signature changes with each build. Thus the permission does not always survive an update: the switch in System Settings can show as on while the permission does not apply. If the viewer finds no streams after an update, set the switch off and then on again, or delete the application, empty the Trash, and install it again to get a new prompt.
- **`xdf_record` is a command-line tool.** It has no bundle of its own, thus macOS holds the permission against the program that started it. When you run the recorder from Terminal, the prompt names Terminal, and a permission that you gave to the viewer does not apply. Permit Terminal (or your terminal application) in the same **Local Network** list.

### Application firewall

If you turned the application firewall on, permit the viewer in **System Settings > Network > Firewall > Options**, or from a terminal:

```sh
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "/Applications/LSL Viewer.app/Contents/MacOS/lsl_viewer"
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "/Applications/LSL Viewer.app/Contents/MacOS/lsl_viewer"
```

## Linux

Debian and Ubuntu leave `ufw` inactive, thus the AppImage needs no change. Fedora, RHEL, and openSUSE start `firewalld` with a default zone that drops inbound traffic, thus LSL finds no streams there until you open the ports.

### firewalld

The repository has a service definition at [packaging/lsl.xml](../packaging/lsl.xml). Install it, and then add the service to your zone:

```sh
sudo cp packaging/lsl.xml /etc/firewalld/services/
sudo firewall-cmd --reload
sudo firewall-cmd --permanent --add-service=lsl
sudo firewall-cmd --reload
```

The third command applies to the default zone. To open the ports on one interface only, name its zone, for example `--zone=internal`. Check the result with `firewall-cmd --list-services`.

If you have the AppImage but not a copy of the repository, add the ports directly instead:

```sh
sudo firewall-cmd --permanent --add-port=16571/udp --add-port=16572-16604/tcp --add-port=16572-16604/udp
sudo firewall-cmd --reload
```

### ufw

```sh
sudo ufw allow 16571/udp
sudo ufw allow 16572:16604/tcp
sudo ufw allow 16572:16604/udp
```

These rules apply to IPv4 and IPv6, which is what liblsl needs with its default `IPv6 = allow`.

To permit your lab subnet only, give the source. But do this for both families, or set `IPv6 = disable` in `lsl_api.cfg` first, because a rule with an IPv4 source does not apply to the IPv6 query:

```sh
sudo ufw allow proto udp from 192.168.1.0/24 to any port 16571
sudo ufw allow proto tcp from 192.168.1.0/24 to any port 16572:16604
sudo ufw allow proto udp from 192.168.1.0/24 to any port 16572:16604
```

`ufw` matches on ports, not on programs, thus these rules apply to all LSL software on the machine, not to the viewer alone.

## Check whether the queries arrive

This applies to Linux and macOS. A firewall problem and a routing problem look the same from the viewer. `socat` tells them apart, because it shows the discovery packets as they arrive. Run it on the machine that cannot see the streams, and start a resolve from another machine:

```sh
socat -d -d UDP-RECV:16571,reuseaddr,broadcast STDOUT
```

A query gives a packet that holds `LSL:shortinfo` and the session ID. If you see the packets here but the viewer finds no streams, the discovery is not the problem: look at the data ports, 16572 to 16604. If you see nothing, the packets do not reach the machine, and the cause is the firewall, the adapter, or the subnet.

To watch one multicast group instead of the broadcasts, join it:

```sh
socat -d -d 'UDP6-RECV:16571,reuseaddr,ipv6-join-group=[ff02:113D:6FDD:2C17:A643:FFE2:1BD1:3CD2]:eth0' STDOUT
```

Give your own interface name: `ip addr` lists them on Linux, and `ifconfig` on macOS.
