#include "dl_fldigi/dl_fldigi.h"

#include <vector>
#include <map>
#include <string>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <set>
#include <json/json.h>
#include <time.h>
#include <Fl/Fl.H>
#include "habitat/UKHASExtractor.h"
#include "habitat/EZ.h"
#include "configuration.h"
#include "debug.h"
#include "fl_digi.h"
#include "confdialog.h"
#include "main.h"

using namespace std;

namespace dl_fldigi {

/* How does online/offline work? if online() is false, uthr->settings() will
 * reset the UploaderThread, leaving it unintialised */

/* TODO: maybe upload the git commit when compiled as the 'version' */

DExtractorManager *extrmgr;
DUploaderThread *uthr;
enum location_mode new_location_mode;
bool show_testing_flights;

static EZ::cURLGlobal *cgl;

static vector<Json::Value> flight_docs;
static vector<string> payload_index;
static bool dl_online, downloaded_once, hab_ui_exists;
static int dirty;
static enum location_mode current_location_mode;
static habitat::UKHASExtractor *ukhas;
static string fldocs_cache_file;

static void flight_docs_init();
static void reset_gps_settings();
static void periodically();
static void select_payload(int index);
static void select_mode(int index);

/* FLTK doesn't provide something like this, as far as I can tell */
class Fl_AutoLock
{
public:
    Fl_AutoLock() { Fl::lock(); };
    ~Fl_AutoLock() { Fl::unlock(); };
};

/* Functions init, ready and cleanup should only be called from main() */
void init()
{
    cgl = new EZ::cURLGlobal();

    uthr = new DUploaderThread();
    extrmgr = new DExtractorManager(*uthr);

    ukhas = new habitat::UKHASExtractor();
    extrmgr->add(*ukhas);

    fldocs_cache_file = HomeDir + "flight_docs.json";
}

void ready(bool hab_mode)
{
    /* if --hab was specified, default online to true, and update ui */
    hab_ui_exists = hab_mode;

    if (progdefaults.gps_start_enabled)
        current_location_mode = LOC_GPS;
    else
        current_location_mode = LOC_STATIONARY;

    flight_docs_init();
    uthr->start();
    reset_gps_settings();
    online(hab_mode);

    /* online will call uthr->settings() if hab_mode since it online will
     * "change" from false to true) */
}

void cleanup()
{
    delete extrmgr;
    extrmgr = 0;

    if (uthr)
    {
        /* When cleaning up the main thread (that executes this function)
         * will hold the lock. Don't deadlock: */
        Fl::unlock();
        uthr->shutdown();
        Fl::lock();
    }

    delete uthr;
    uthr = 0;

    delete cgl;
    cgl = 0;
}

static void periodically()
{
    /* TODO: arrange for this to be called periodically by fltk */

    uthr->listener_info();

    if (current_location_mode == LOC_STATIONARY)
        uthr->listener_telemetry();
}

/* All other functions should hopefully be thread safe */
void online(bool val)
{
    Fl_AutoLock lock;

    bool changed;

    changed = (dl_online != val);
    dl_online = val;

    if (changed)
    {
        uthr->settings();
    }

    if (changed && dl_online)
    {
        if (!downloaded_once)
            uthr->flights();

        uthr->listener_info();
        uthr->listener_telemetry();
    }

    confdialog_dl_online->value(val);
    set_menu_dl_online(val);
}

bool online()
{
    Fl_AutoLock lock;
    return dl_online;
}

static void flight_docs_init()
{
    ifstream cf(fldocs_cache_file.c_str());

    if (cf.fail())
    {
        LOG_DEBUG("Failed to open cache file");
        return;
    }

    flight_docs.clear();

    while (cf.good())
    {
        string line;
        getline(cf, line, '\n');

        char discard;
        while (cf.good() && cf.peek() == '\n')
            cf.get(discard);

        Json::Reader reader;
        Json::Value root;
        if (!reader.parse(line, root, false))
            break;

        flight_docs.push_back(root);
    }

    bool failed = cf.fail() || !cf.eof();

    cf.close();

    if (failed)
    {
        flight_docs.clear();
        LOG_WARN("Failed to load flight doc cache from file");
    }
    else
    {
        LOG_DEBUG("Loaded %li flight docs from file", flight_docs.size());
    }

    populate_flights();
}

static string escape_menu_string(const string &s_)
{
    string s(s_);
    size_t pos = 0;

    do
    {
        pos = s.find_first_of("&/\\_", pos);

        if (pos != string::npos)
        {
            s.insert(pos, 1, '\\');
            pos += 2;
        }
    }
    while (pos != string::npos);

    return s;
}

static string escape_browser_string(const string &s_)
{
    string s(s_);
    size_t pos = 0;

    do
    {
        pos = s.find('\t', pos);
        if (pos != string::npos)
            s[pos] = ' ';
    }
    while (pos != string::npos);

    return s;
}

static string get_payload_list(const Json::Value &flight)
{
    const Json::Value &payloads = flight["payloads"];

    if (!payloads.isObject() || !payloads.size())
        return "";

    const vector<string> payload_names = payloads.getMemberNames();
    ostringstream payload_list;
    vector<string>::const_iterator it;

    for (it = payload_names.begin(); it != payload_names.end(); it++)
    {
        if (it != payload_names.begin())
            payload_list << ',';
        payload_list << (*it);
    }

    return payload_list.str();
}

static string flight_launch_date(const Json::Value &flight)
{
    const Json::Value &launch = flight["launch"];

    if (!launch.isObject() || !launch.size())
        return "";

    if (!launch.isMember("time") || !launch["time"].isInt())
        return "";

    time_t date = launch["time"].asInt();
    char buf[20];
    struct tm tm;

    if (gmtime_r(&date, &tm) != &tm)
        return "";

    if (strftime(buf, sizeof(buf), "%a %d %b %y", &tm) <= 0)
        return "";

    return buf;
}

static bool is_testing_flight(const Json::Value &flight)
{
    /* TODO: is this a testing flight? */
    /* Crude test: */
    return !(flight["end"].isInt() && flight["end"].asInt());
}

static string flight_choice_item(const string &name,
                                 const string &payload_list,
                                 int attempt)
{
    /* "<name>: <payload>,<payload>,<payload>" */

    string attempt_suffix;

    if (attempt != 1)
    {
        ostringstream sfx;
        sfx << " (" << attempt << ")";
        attempt_suffix = sfx.str();
    }

    return escape_menu_string(name + ": " + payload_list + attempt_suffix);
}

static string flight_browser_item(const string &name, const string &date,
                                  const string &payload_list)
{
    /* "<name>\t<optional date>\t<payload>,<payload>" */
    return "@." + escape_browser_string(name) + "\t" +
           "@." + date + "\t" +
           "@." + escape_browser_string(payload_list);
}

static void flight_choice_callback(Fl_Widget *w, void *a)
{
    int index = reinterpret_cast<intptr_t>(a);
    Fl_Choice *choice = static_cast<Fl_Choice *>(w);
    flight_browser->value(choice->value() + 1);
    select_flight(index);
}

void populate_flights()
{
    Fl_AutoLock lock;

    set<string> choice_items;

    if (hab_ui_exists)
    {
        habFlight->value(-1);
        habFlight->clear();
    }

    flight_browser->clear();

    select_flight(-1);

    vector<Json::Value>::const_iterator it;
    intptr_t i;
    for (it = flight_docs.begin(), i = 0; it != flight_docs.end(); it++, i++)
    {
        const Json::Value &root = (*it);

        if (!root.isObject() || !root.size() ||
            !root["_id"].isString() || !root["name"].isString())
        {
            LOG_WARN("invalid flight doc");
            continue;
        }

        const string id = root["_id"].asString();
        const string name = root["name"].asString();
        const string payload_list = get_payload_list(root);
        const string date = flight_launch_date(root);
        void *userdata = reinterpret_cast<void *>(i);

        if (!id.size() || !name.size() || !payload_list.size())
        {
            LOG_WARN("invalid flight doc");
            continue;
        }

        if (is_testing_flight(root) && !show_testing_flights)
            continue;

        if (hab_ui_exists)
        {
            string item;
            int attempt = 1;

            /* Avoid duplicate menu items: fltk removes them */
            do
            {
                item = flight_choice_item(name, payload_list, attempt);
                attempt++;
            }
            while (choice_items.count(item));
            choice_items.insert(item);

            habFlight->add(item.c_str(), (int) 0, flight_choice_callback,
                           userdata);
        }

        string browser_item = flight_browser_item(name, date, payload_list);
        flight_browser->add(browser_item.c_str(), userdata);

        if (progdefaults.tracking_flight == id)
        {
            if (hab_ui_exists)
                habFlight->value(habFlight->size() - 2);
            flight_browser->value(flight_browser->size());
            select_flight(i);
        }
    }
}

static void payload_choice_callback(Fl_Widget *w, void *a)
{
    Fl_Choice *choice = static_cast<Fl_Choice *>(w);
    Fl_Choice *other = static_cast<Fl_Choice *>(a);

    if (other)
        other->value(choice->value());
    select_payload(choice->value());
}

void select_flight(int index)
{
    Fl_AutoLock lock;

    LOG_DEBUG("Selecting flight, index %i", index);

    if (hab_ui_exists)
    {
        habCHPayload->value(-1);
        habCHPayload->clear();
        habCHPayload->deactivate();
    }

    payload_list->value(-1);
    payload_list->clear();
    payload_list->deactivate();

    select_payload(-1);

    int max = flight_docs.size();
    if (index < 0 || index >= max)
        return;

    const Json::Value &flight = flight_docs[index];

    if (!flight.isObject() || !flight.size() || !flight["_id"].isString())
        return;

    const string id = flight["_id"].asString();
    const Json::Value &payloads = flight["payloads"];

    if (!id.size() || !payloads.isObject() || !payloads.size())
        return;

    payload_index = payloads.getMemberNames();
    int auto_select = 0;

    vector<string>::const_iterator it;
    int i;
    for (it = payload_index.begin(), i = 0;
         it != payload_index.end();
         it++, i++)
    {
        if (hab_ui_exists)
            habCHPayload->add(escape_menu_string(*it).c_str(), (int) 0,
                              payload_choice_callback, payload_list);
        payload_list->add(escape_menu_string(*it).c_str(), (int) 0,
                          payload_choice_callback, habCHPayload);

        if ((*it) == progdefaults.tracking_payload)
            auto_select = i;
    }

    if (hab_ui_exists)
    {
        habCHPayload->activate();
        habCHPayload->value(auto_select);
    }

    payload_list->activate();
    payload_list->value(auto_select);

    if (progdefaults.tracking_flight != id)
    {
        progdefaults.tracking_flight = id;
        progdefaults.changed = true;
    }

    select_payload(auto_select);
}

static void select_payload(int index)
{
    Fl_AutoLock lock;
    LOG_DEBUG("Selecting payload %i", index);

    if (hab_ui_exists)
    {
        habCHMode->value(-1);
        habCHMode->clear();
        habCHMode->deactivate();
    }

    payload_mode_list->value(-1);
    payload_mode_list->clear();
    payload_mode_list->deactivate();

    select_mode(-1);

    int max = payload_index.size();
    if (index < 0 || index >= max)
        return;

    const string name(payload_index[index]);
    if (progdefaults.tracking_payload != name)
    {
        progdefaults.tracking_payload = name;
        progdefaults.changed = true;
    }

    /* TODO: select payload */
}

static void select_mode(int index)
{
    Fl_AutoLock lock;

    if (hab_ui_exists)
    {
        habConfigureButton->deactivate();
        habSwitchModes->deactivate();
    }

    payload_autoconfigure->deactivate();

    /* TODO: select mode */
}

static void reset_gps_settings()
{
    /* if (gps_thread != NULL)  gps_thread->join(); delete gps_thread; */

    if (current_location_mode == LOC_GPS)
    {
        /* TODO gps_thread = new stuff */
    }
}

void changed(enum changed_groups thing)
{
    Fl_AutoLock lock;
    dirty |= thing;
}

void commit()
{
    Fl_AutoLock lock;

    /* Update something if its settings change; fairly simple: */
    if (dirty & CH_UTHR_SETTINGS)
    {
        downloaded_once = false;

        uthr->settings();
        uthr->flights();
    }
    
    if (dirty & CH_LOCATION_MODE)
    {
        current_location_mode = new_location_mode;
    }

    if ((dirty & CH_LOCATION_MODE) || (dirty & CH_GPS_SETTINGS))
    {
        reset_gps_settings();
    }

    /* If the info has been updated, or the upload settings changed... */
    if (dirty & (CH_UTHR_SETTINGS | CH_INFO))
    {
        uthr->listener_info();
    }

    /* if stationary and (settings changed, or if we just switched to
     * stationary mode from gps mode, or if the upload settings changed) */
    if (current_location_mode == LOC_STATIONARY &&
        (dirty & (CH_STATIONARY_LOCATION | CH_LOCATION_MODE |
                  CH_UTHR_SETTINGS)))
    {
        uthr->listener_telemetry();
    }

    dirty = CH_NONE;
}

void DExtractorManager::status(const string &msg)
{
    Fl_AutoLock lock;

    LOG_DEBUG("hbtE %s", msg.c_str());
    /* TODO: Log message from extractor */
    /* TODO: put_status safely */
}

void DExtractorManager::data(const Json::Value &d)
{
    Fl_AutoLock lock;

    if (!hab_ui_exists)
        return;

    /* TODO: Data to fill out HAB UI */
}

/* TODO: abort these if critical settings are missing and don't upload
 * empty string values (e.g., "antenna": ""). */

/* All these functions are called via a DUploaderThread pointer so
 * the fact that they are non virtual is OK. Having a different set of
 * arguments prevents the wrong function from being called except in the
 * case of flights() */

void DUploaderThread::settings()
{
    Fl_AutoLock lock;

    UploaderThread::reset();

    if (!online())
    {
        warning("upload disabled: offline");
        return;
    }

    if (!progdefaults.myCall.size() || !progdefaults.habitat_uri.size() ||
        !progdefaults.habitat_db.size())
    {
        warning("upload disabled: settings missing");
        return;
    }

    UploaderThread::settings(progdefaults.myCall, progdefaults.habitat_uri,
                             progdefaults.habitat_db);
}

/* This function is used for stationary listener telemetry */
void DUploaderThread::listener_telemetry()
{
    Fl_AutoLock lock;

    if (current_location_mode != LOC_STATIONARY)
    {
        warning("attempted to upload stationary listener "
                "telemetry while in GPS telemetry mode");
        return;
    }

    if (!progdefaults.myLat.size() || !progdefaults.myLon.size())
    {
        warning("unable to upload stationary listener telemetry: "
                "latitude or longitude missing");
        return;
    }

    double latitude, longitude;
    istringstream lat_strm(progdefaults.myLat), lon_strm(progdefaults.myLon);
    lat_strm >> latitude;
    lon_strm >> longitude;

    if (lat_strm.fail())
    {
        warning("unable to parse stationary latitude");
        return;
    }

    if (lon_strm.fail())
    {
        warning("unable to parse stationary longitude");
        return;
    }

    Json::Value data(Json::objectValue);

    /* TODO: is it really a good idea to upload time like this? */
    struct tm tm;
    time_t now;

    now = time(NULL);
    if (now < 0)
        throw runtime_error("time() failed");

    struct tm *tm_p = gmtime_r(&now, &tm);
    if (tm_p != &tm)
        throw runtime_error("gmtime() failed");

    data["time"] = Json::Value(Json::objectValue);
    Json::Value &time = data["time"];
    time["hour"] = tm.tm_hour;
    time["minute"] = tm.tm_min;
    time["second"] = tm.tm_sec;

    data["latitude"] = latitude;
    data["longitude"] = longitude;

    UploaderThread::listener_telemetry(data);
}

static void info_add(Json::Value &data, const string &key, const string &value)
{
    if (value.size())
        data[key] = value;
}

void DUploaderThread::listener_info()
{
    Fl_AutoLock lock;

    Json::Value data(Json::objectValue);
    info_add(data, "name", progdefaults.myName);
    info_add(data, "location", progdefaults.myQth);
    info_add(data, "radio", progdefaults.myRadio);
    info_add(data, "antenna", progdefaults.myAntenna);

    if (!data.size())
    {
        warning("not uploading empty listener info");
        return;
    }

    UploaderThread::listener_info(data);
}

/* These functions must try to be thread safe. */
void DUploaderThread::log(const string &message)
{
    Fl_AutoLock lock;
    LOG_DEBUG("hbtUT %s", message.c_str());
    /* TODO: put_status safely */
}

void DUploaderThread::warning(const string &message)
{
    Fl_AutoLock lock;
    LOG_WARN("hbtUT %s", message.c_str());
    /* TODO: put_status safely & kick up a fuss */
}

void DUploaderThread::got_flights(const vector<Json::Value> &new_flight_docs)
{
    Fl_AutoLock lock;

    flight_docs = new_flight_docs;

    ostringstream ltmp;
    ltmp << "Downloaded " << new_flight_docs.size() << " flight docs";
    log(ltmp.str());

    flight_docs = new_flight_docs;
    downloaded_once = true;

    ofstream cf(fldocs_cache_file.c_str(), ios_base::out | ios_base::trunc);

    for (vector<Json::Value>::const_iterator it = flight_docs.begin();
         it != flight_docs.end() && cf.good();
         it++)
    {
        Json::FastWriter writer;
        cf << writer.write(*it);
    }

    bool success = cf.good();

    cf.close();

    if (!success)
    {
        warning("unable to save flights data");
        unlink(fldocs_cache_file.c_str());
    }

    populate_flights();
}

} /* namespace dl_fldigi */
