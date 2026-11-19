#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>
#include <vector>

struct tagINPUT;

namespace evp {

// One instance owns one attended input session. Use it only on the host controller's thread.
class HostInput final {
  public:
    using Sender = std::function<bool(const tagINPUT&)>;
    using EnvironmentCheck = std::function<QString()>;
    enum class Result { Ignored, Applied, Failed };

    explicit HostInput(Sender sender = {}, EnvironmentCheck environmentCheck = {});
    ~HostInput();
    HostInput(const HostInput&) = delete;
    HostInput& operator=(const HostInput&) = delete;

    QString checkEnvironment() const;
    QString arm();
    // The controller authenticates the session and epoch before passing the event here.
    Result apply(const QJsonObject& event);
    bool disarm();

  private:
    bool releaseAll();
    Result fail();

    Sender sender_;
    EnvironmentCheck environmentCheck_;
    std::vector<int> heldKeys_;
    std::vector<int> heldButtons_;
    bool armed_ = false;
};

} // namespace evp
