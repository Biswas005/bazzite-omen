# Bazzite Omen 🎮⚡

A custom [Bazzite](https://bazzite.gg/) image optimized for **HP Omen gaming laptops**, built using [bootc](https://github.com/bootc-dev/bootc) technology. This image provides enhanced gaming performance, better hardware compatibility, and **seamless thermal management directly through KDE's power widgets** - no additional software required!

## 🎯 What is Bazzite Omen?

Bazzite Omen is a specialized Linux gaming distribution that combines the power of Bazzite with specific optimizations for HP Omen hardware. It's built on Fedora Atomic Desktops and provides:

- **Gaming-first approach** with Steam, Lutris, and gaming tools pre-installed
- **🔥 Revolutionary Thermal Control** - Manage HP Omen thermal profiles directly via KDE power slider
- **Immutable OS design** for stability and reliability
- **Container-based architecture** for easy updates and rollbacks
- **Ready-to-game experience** with minimal setup required

## ✨ Features

### 🎮 Gaming Optimizations
- Steam, Lutris, and Heroic Games Launcher pre-installed
- Gaming-optimized kernel with performance governors
- Proton/Wine configured for maximum compatibility
- GameMode integration for performance optimization
- Discord and gaming communication tools included

### 🔥 **Unique Thermal Integration**
- **Direct KDE Power Widget Control** - Adjust thermal profiles instantly via the native KDE power mode slider
- **No External Software Required** - Thermal management integrated directly into the desktop environment
- **Real-time Adjustments** - Switch between Silent, Balanced, and Performance modes on-the-fly
- **Visual Feedback** - Instant temperature and fan speed updates in the system tray

### 🖥️ HP Omen Specific Features
- **🔥 Integrated Thermal Control** - **Direct thermal management via KDE power mode slider** - no additional software needed!
- **RGB Lighting Control** - Full support for Omen RGB keyboards and chassis lighting
- **Native Power Profiles** - Seamlessly integrated with KDE's power management widgets
- **Hardware Monitoring** - Real-time temperature and performance monitoring in system tray
- **Omen Command Center Alternative** - Linux-native tools for complete Omen hardware management

### 🔧 System Features
- **Immutable OS** - Reliable, predictable system state
- **Atomic Updates** - Safe system updates with rollback capability
- **Container Integration** - Podman and Docker support
- **Development Tools** - Essential development packages included
- **Multimedia Support** - Full codec support for media playback

## 🚀 Quick Start

### Prerequisites
- HP Omen gaming laptop (recommended)
- UEFI/Secure Boot compatible system
- At least 4GB RAM (8GB+ recommended)
- 50GB+ available storage

### Installation Methods

#### Option 1: ISO Installation (Recommended)
1. Download the latest ISO from [Releases](https://github.com/Biswas005/bazzite-omen/releases)
2. Create a bootable USB drive using [Rufus](https://rufus.ie/) or [Balena Etcher](https://www.balena.io/etcher/)
3. Boot from USB and follow the installation wizard
4. Reboot and enjoy your gaming-optimized system!

#### Option 2: Rebase from Existing Bazzite
```bash
# If you already have Bazzite installed
rpm-ostree rebase --experimental ostree-unverified-registry:ghcr.io/biswas005/bazzite-omen:latest
systemctl reboot
```

#### Option 3: Container Image
```bash
# Pull the container image
podman pull ghcr.io/biswas005/bazzite-omen:latest

# Run the container
podman run -it ghcr.io/biswas005/bazzite-omen:latest
```
## Post Installation
```bash
 rpm-ostree rebase ostree-image-signed:docker://ghcr.io/biswas005/bazzite-omen:latest
```
## 🛠️ Development & Customization

### Building from Source

#### Prerequisites
- Linux system with Podman or Docker
- Git
- Just (command runner)

#### Build Steps
```bash
# Clone the repository
git clone https://github.com/Biswas005/bazzite-omen.git
cd bazzite-omen

# Build the container image
just build

# Build ISO image
just build-iso

# Build VM image for testing
just build-vm
```

### Customization Options

#### Modify Hardware Support
Edit the `Containerfile` to add or remove hardware-specific packages:
```dockerfile
# Add additional drivers or tools
RUN rpm-ostree install \
    your-package-here \
    another-package
```

#### Gaming Configuration
Customize gaming settings in `files/gaming-config/`:
- Steam configuration
- Lutris settings
- Performance profiles
- Controller configurations

#### Omen-Specific Tweaks
Modify Omen hardware support in `files/omen-config/`:
- RGB lighting profiles
- Thermal management scripts
- Performance governors
- Hardware monitoring tools

### Available Just Commands

```bash
# Development
just check-just        # Check Justfile syntax
just fix-just          # Fix Justfile formatting
just clean             # Clean build artifacts

# Building
just build             # Build container image
just build-iso         # Build ISO image
just build-vm          # Build VM image (QCOW2)
just build-raw         # Build RAW disk image

# Testing
just run-vm            # Run VM for testing
just spawn-vm          # Use systemd-vmspawn for testing

# Code Quality
just shellcheck        # Check shell scripts
just shfmt             # Format shell scripts
```

## 📦 What's Included

### Base System
- **Fedora Atomic Desktop** - Reliable, immutable base
- **KDE Plasma** or **GNOME** desktop environment
- **Flatpak** - Sandboxed application support
- **Podman** - Container management

### Gaming Stack
- **Steam** - Primary gaming platform
- **Lutris** - Gaming library manager
- **Heroic Games Launcher** - Epic Games Store client
- **GameMode** - Performance optimization
- **MangoHud** - Gaming performance overlay
- **Discord** - Gaming communication

### HP Omen Tools
- **OpenRGB** - RGB lighting control
- **Fancontrol** - Thermal management
- **Sensors** - Hardware monitoring
- **Power Profiles Daemon** - Power management
- **Custom Omen Scripts** - Hardware integration tools

### Development & Utilities
- **Git** - Version control
- **VS Code** - Code editor (via Flatpak)
- **Firefox** - Web browser
- **Multimedia codecs** - Full media support
- **System monitoring tools** - Performance tracking

## 🔧 Post-Install Configuration

### Gaming Setup
1. **Steam**: Launch Steam and sign in to your account
2. **Proton**: Enable Proton for Windows games in Steam settings
3. **Lutris**: Configure wine runners and gaming libraries
4. **Controllers**: Connect gaming controllers - they should work out of the box

### Omen Hardware Setup
1. **🔥 Thermal Control**: Use the **KDE power mode slider** in your system tray to instantly switch between:
   - **Silent Mode** - Quiet operation for productivity and light tasks
   - **Balanced Mode** - Optimal balance of performance and acoustics
   - **Performance Mode** - Maximum cooling for intensive gaming sessions
2. **RGB Lighting**: Use OpenRGB or included Omen RGB tools
3. **Real-time Monitoring**: Check temperatures and fan speeds directly in KDE system monitor widgets
4. **Display Settings**: Configure refresh rates and resolution for gaming

### System Optimization
```bash
# Update system
rpm-ostree upgrade

# Install additional Flatpaks
flatpak install flathub com.discordapp.Discord
flatpak install flathub org.gimp.GIMP

# Configure gaming performance
systemctl --user enable gamemoded
```

## 🤝 Contributing

We welcome contributions! Here's how you can help:

### Reporting Issues
- Use GitHub Issues for bug reports
- Include system information and logs
- Describe expected vs actual behavior

### Contributing Code
1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

### Areas for Contribution
- HP Omen hardware support improvements
- Gaming performance optimizations
- Documentation updates
- Bug fixes and stability improvements
- New features and tools

## 📚 Documentation & Support

### Getting Help
- **Issues**: [GitHub Issues](https://github.com/Biswas005/bazzite-omen/issues)
- **Discussions**: [GitHub Discussions](https://github.com/Biswas005/bazzite-omen/discussions)
- **Bazzite Community**: [Discord](https://discord.gg/bazzite)
- **bootc Forums**: [Discussions](https://github.com/bootc-dev/bootc/discussions)

### Related Projects
- **[Bazzite](https://bazzite.gg/)** - Main gaming distribution
- **[Universal Blue](https://universal-blue.org/)** - Cloud-native desktop project
- **[bootc](https://github.com/bootc-dev/bootc)** - Container-based OS technology

## 🔒 Security & Signing

This image is signed with Cosign for supply chain security. To verify the signature:

```bash
# Install cosign
curl -O -L "https://github.com/sigstore/cosign/releases/latest/download/cosign-linux-amd64"
sudo mv cosign-linux-amd64 /usr/local/bin/cosign
sudo chmod +x /usr/local/bin/cosign

# Verify the image
cosign verify --key cosign.pub ghcr.io/biswas005/bazzite-omen:latest
```

## 📄 License

This project is licensed under the Apache License 2.0. See [LICENSE](LICENSE) for details.

## 🙏 Acknowledgments

- **[Bazzite Team](https://github.com/ublue-os/bazzite)** - For the amazing gaming distribution
- **[Universal Blue](https://github.com/ublue-os)** - For the cloud-native desktop ecosystem
- **HP Omen Community** - For hardware insights and testing
- **Fedora Project** - For the solid foundation

## 🚨 Disclaimer

This is an unofficial project not affiliated with HP or the official Bazzite project. HP Omen is a trademark of HP Inc. Use at your own risk and always backup your data before installation.

---

**Ready to game on Linux with your HP Omen? Let's go! 🎮🚀**
