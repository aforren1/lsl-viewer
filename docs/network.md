# Network and firewall

LSL finds streams with UDP multicast and transfers the data with TCP. Thus the viewer must accept inbound traffic, and the sources must be on the same subnet. This page tells you which ports LSL uses, what the Windows installer does, and how to correct a machine that cannot see the streams.

For the LSL side of the subject, see the [LSL network connectivity guide](https://labstreaminglayer.readthedocs.io/info/network-connectivity.html).

## Ports

| Port | Protocol | Use |
|---|---|---|
| 16571 | UDP | Service discovery. A resolver sends its query here, by multicast and by unicast, and each provider listens here and replies. |
| 16572 to 16604 | TCP | Stream data. Each outlet takes a free port from the range. |
| 16572 to 16604 | UDP | Clock synchronization between an outlet and its inlets. |
| 22345 | TCP | The viewer remote-control port, if you turn it on. See the [Remote control](../README.md#remote-control) section. |

The viewer is both a consumer and a provider: it reads the streams, and it announces its own control endpoint as an LSL stream. Thus it needs the inbound rules that a source needs, not only the rules that a recorder needs.

Do not open the ports one by one. Permit the program instead, because liblsl selects a port from a range at run time, and the control listener can move to a port that the operating system gives it.

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

3. Check that the machines are on the same subnet. Multicast does not cross a router unless the network is configured for it. If they are on different subnets, set `KnownPeers` in the [LSL configuration file](https://labstreaminglayer.readthedocs.io/info/lslapicfg.html) to list the source machines by address.

**Only some streams appear.** The resolve traffic and the data traffic use different ports. If the streams show in the list but do not connect, the rule permits UDP but not TCP. A program rule with no protocol covers both; a port rule does not.

## Linux and macOS

Neither platform blocks inbound traffic by default on a typical desktop install.

- **macOS** raises a prompt for incoming connections the first time you run the viewer. Select **Allow**. To correct a refusal, use System Settings > Network > Firewall > Options.
- **Linux** with `ufw` or `firewalld` active needs the LSL ports. For example, with `ufw`:

  ```sh
  sudo ufw allow proto udp from 192.168.1.0/24 to any port 16571:16604
  sudo ufw allow proto tcp from 192.168.1.0/24 to any port 16572:16604
  ```

  Give your own subnet.
