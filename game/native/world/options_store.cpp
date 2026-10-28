#include "options_store.hpp"
#include "forced_time.hpp"
#include <fstream>
#include <stdexcept>
#include <utility>

namespace r1::world {
namespace fs = std::filesystem;
using json = nlohmann::json;

OptionsStore::OptionsStore(fs::path project, Reporter reporter)
    : path_(std::move(project)/"cache/options.json"), reporter_(std::move(reporter)) {}

void OptionsStore::report(Level level, const std::string& message) const {
    if(reporter_)reporter_(level,message);
}

SavedOptions OptionsStore::load(SavedOptions current, bool testRun) const {
    std::ifstream input(path_);
    if(!input)return current;
    if(testRun){report(Level::Info,"cache/options.json ignored by a test run");return current;}
    try {
        const json options=json::parse(input);
        if(!options.is_object())throw std::runtime_error("options must be an object");
        if(const auto hour=options.find("forcedTime");hour!=options.end()) {
            if(!hour->is_string())report(Level::Error,"cache/options.json: forcedTime must be a string, ignored");
            else {
                const auto typed=hour->get<std::string>();
                if(!typed.empty()) {
                    current.forcedMinutes=r1::parseClockTime(typed);
                    if(current.forcedMinutes)report(Level::Info,"time forced to "+r1::formatClockTime(*current.forcedMinutes)+" local");
                    else report(Level::Error,"cache/options.json: forcedTime '"+typed+"' is not HH:MM, ignored");
                }
            }
        }
        if(const auto atmosphere=options.find("forcedWeather");atmosphere!=options.end()) {
            const auto mode=atmosphere->is_string()?r1::parseWeatherOverride(atmosphere->get<std::string>()):std::nullopt;
            if(mode) {
                current.forcedWeather=*mode;
                report(Level::Info,std::string("weather mode ")+r1::weatherOverrideKey(current.forcedWeather)+" everywhere");
            } else report(Level::Error,"cache/options.json: invalid forcedWeather, ignored");
        }
        if(const auto toaster=options.find("toasterMode");toaster!=options.end()) {
            if(toaster->is_boolean()) {
                current.toasterMode=toaster->get<bool>();
                report(Level::Info,std::string("toasterMode ")+(current.toasterMode?"enabled":"disabled"));
            } else report(Level::Error,"cache/options.json: invalid toasterMode, ignored");
        }
        if(const auto fullscreen=options.find("fullscreen");fullscreen!=options.end()) {
            if(fullscreen->is_boolean()) {
                current.fullscreen=fullscreen->get<bool>();
                report(Level::Info,std::string("fullscreen ")+(current.fullscreen?"enabled":"disabled"));
            } else report(Level::Error,"cache/options.json: invalid fullscreen, ignored");
        }
        if(const auto gi=options.find("giQuality");gi!=options.end()) {
            if(gi->is_number_integer()&&*gi>=-1&&*gi<=4)current.giQuality=gi->get<int>();
            else report(Level::Error,"cache/options.json: invalid giQuality, ignored");
        }
    } catch(const std::exception& e) {
        report(Level::Error,std::string("cache/options.json unreadable, ignored: ")+e.what());
    }
    return current;
}

bool OptionsStore::save(const SavedOptions& values, bool testRun) const {
    if(testRun)return true;
    json options=json::object();
    // Preserve unrecognized keys so updating one setting cannot erase future ones.
    std::ifstream input(path_);
    if(input) {
        try {
            auto saved=json::parse(input);
            if(saved.is_object())options=std::move(saved);
            else report(Level::Warning,"replacing invalid non-object options");
        } catch(const std::exception& e) {
            report(Level::Warning,std::string("replacing unreadable options: ")+e.what());
        }
        input.close();
    }
    if(values.forcedMinutes)options["forcedTime"]=r1::formatClockTime(*values.forcedMinutes);
    else options.erase("forcedTime");
    options["forcedWeather"]=r1::weatherOverrideKey(values.forcedWeather);
    options["toasterMode"]=values.toasterMode;
    options["giQuality"]=values.giQuality;
    options["fullscreen"]=values.fullscreen;
    std::error_code ignored;fs::create_directories(path_.parent_path(),ignored);
    std::ofstream output(path_);
    output<<options.dump(2)<<"\n";
    output.flush();
    if(output)return true;
    report(Level::Error,"could not write "+path_.string());
    return false;
}

} // namespace r1::world
