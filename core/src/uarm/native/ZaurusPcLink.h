#ifndef FROND_ZAURUS_PC_LINK_H
#define FROND_ZAURUS_PC_LINK_H

#include <memory>
#include <string>

class SocPXA;
class UsbHostBridge;

class ZaurusPcLink {
  public:
    struct Reply { bool ok; std::string code, message; };

    explicit ZaurusPcLink(std::string controlPath);
    ~ZaurusPcLink();
    Reply Command(UsbHostBridge& bridge, SocPXA& soc, const std::string& command, const std::string& argument);
    void BeginClose();
    bool FinishClose();
    void Close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

#endif
