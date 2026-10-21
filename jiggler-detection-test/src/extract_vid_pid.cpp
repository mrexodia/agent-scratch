// ExtractVidPid() copied from detect_my_jiggler/device_utils.cpp (MIT, see
// third_party/detect_my_jiggler/LICENSE). The rest of that file is Windows-only.

#include <algorithm>
#include <cctype>
#include <string>

void ExtractVidPid(const std::string& deviceName, std::string& vid, std::string& pid) {
    std::string upper = deviceName;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c){ return (char)std::toupper(c); });
    
    // Try VID_ format first (most common)
    auto vidPos = upper.find("VID_");
    if (vidPos != std::string::npos && vidPos + 8 <= upper.size()) {
        vid = upper.substr(vidPos + 4, 4);
    }
    
    // Try PID_ format first (most common)
    auto pidPos = upper.find("PID_");
    if (pidPos != std::string::npos && pidPos + 8 <= upper.size()) {
        pid = upper.substr(pidPos + 4, 4);
    }
    
    // Alternative formats: VID& or VID# 
    if (vid.empty()) {
        vidPos = upper.find("VID&");
        if (vidPos == std::string::npos) vidPos = upper.find("VID#");
        if (vidPos != std::string::npos && vidPos + 8 <= upper.size()) {
            vid = upper.substr(vidPos + 4, 4);
        }
    }
    
    if (pid.empty()) {
        pidPos = upper.find("PID&");
        if (pidPos == std::string::npos) pidPos = upper.find("PID#");
        if (pidPos != std::string::npos && pidPos + 8 <= upper.size()) {
            pid = upper.substr(pidPos + 4, 4);
        }
    }
}
