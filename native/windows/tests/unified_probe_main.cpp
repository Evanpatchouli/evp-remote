#include <QByteArray>
#include <QtGlobal>

int runHostProbe(int argc, char** argv);
int runViewerProbe(int argc, char** argv);

int main(int argc, char** argv) {
    const auto role = qgetenv("EVP_PROBE_ROLE");
    if (role == "host")
        return runHostProbe(argc, argv);
    if (role == "viewer")
        return runViewerProbe(argc, argv);
    return 2;
}
