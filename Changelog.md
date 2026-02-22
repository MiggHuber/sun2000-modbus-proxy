
---

# 📄 CHANGELOG.md

```markdown
# Changelog

All notable changes to this project will be documented in this file.

---

## [1.0.0] - Initial stable working version

### Added
- Working Modbus TCP Proxy
- Huawei SUN2000 TCP client connection
- Grace period after connect
- Timeout protection
- Write job queue
- Poll block handling
- Safe reconnect logic

### Fixed
- Dongle crash due to aggressive polling
- Request timeout lockups
- TCP reconnect instability

---

## Next

- Remove register mapping
- Implement full transparent 1:1 forwarding
- Optimize memory usage