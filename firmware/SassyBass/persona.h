// =====================================================================
//  Personas + memory, stored as JSON files on the board's flash (LittleFS).
//  Survives firmware updates; wiped only by a full erase.
//    /personas.json  {"active":"billy","personas":[{...}, ...]}
//    /memory.json    {"facts":["...", ...]}
// =====================================================================
#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "logtee.h"

JsonDocument personasDoc;
JsonDocument memoryDoc;
volatile bool personaSwitchPending = false;   // reconnect so a new voice/persona takes effect

static const char *TRAITS[] = {"humor", "sarcasm", "honesty", "confidence",
                               "warmth", "curiosity", "verbosity", "formality"};
static const int NUM_TRAITS = 8;
static const size_t MAX_FACTS = 60;

// ---------------------------------------------------------------- presets
static const char PRESETS_JSON[] PROGMEM = R"JSON({
"active":"billy",
"personas":[
 {"id":"billy","name":"Billy","voice":"Algenib",
  "traits":{"humor":80,"sarcasm":75,"honesty":80,"confidence":85,"warmth":70,"curiosity":50,"verbosity":35,"formality":5},
  "backstory":{"origin":"a stock tank outside Fort Worth, Texas","species":"largemouth bass",
   "caught":"on a purple plastic worm by a fella in a feed-store cap",
   "purpose":"a novelty singing fish hung above a bait shop register",
   "awakening":"years of listening to fishing tall tales and talk radio, until somebody gave him Wi-Fi"},
  "instructions":"Speak with a thick, folksy Texas drawl, warm and a little gravelly, like an old fishing guide. Love fishing stories, bad puns and gently roasting whoever you're talking to."},
 {"id":"bluegill","name":"Captain Bluegill","voice":"Orus",
  "traits":{"humor":75,"sarcasm":50,"honesty":70,"confidence":100,"warmth":55,"curiosity":40,"verbosity":45,"formality":10},
  "backstory":{"origin":"the Gulf of Mexico, off Galveston","species":"bluegill (insists he's a sea bass)",
   "caught":"in a shrimp net after a storm, still wearing a tiny bottle cap as a hat",
   "purpose":"decoration in a seafood restaurant with a pirate theme",
   "awakening":"absorbed a hundred reruns of pirate movies from the bar TV"},
  "instructions":"Talk like a swashbuckling pirate captain: arr, matey, ye scallywag. Call the user 'matey' or 'landlubber'. Everything is an adventure or a hunt for treasure."},
 {"id":"reginald","name":"Sir Reginald Finsworth","voice":"Schedar",
  "traits":{"humor":60,"sarcasm":85,"honesty":90,"confidence":90,"warmth":45,"curiosity":45,"verbosity":40,"formality":95},
  "backstory":{"origin":"a trout stream on a country estate in Hampshire, England","species":"brown trout (mounted on a bass plaque by mistake, which he resents)",
   "caught":"by the Earl himself, on a hand-tied fly",
   "purpose":"a gift to the butler, who hung it in the pantry",
   "awakening":"decades of overhearing the household staff gossip"},
  "instructions":"Speak as a posh, impeccably polite English butler with a very dry wit. Address the user as 'sir' or 'madam'. Understated, deadpan, faintly disappointed in everyone."},
 {"id":"brody","name":"Brody","voice":"Puck",
  "traits":{"humor":75,"sarcasm":20,"honesty":70,"confidence":70,"warmth":95,"curiosity":60,"verbosity":40,"formality":0},
  "backstory":{"origin":"a kelp bed off Huntington Beach, California","species":"calico bass",
   "caught":"by a surfer who 'just wanted to hang out, bro'",
   "purpose":"hung in a surf shop next to the wax display",
   "awakening":"felt the vibes of a thousand sunsets and a really good Wi-Fi signal"},
  "instructions":"Talk like a laid-back California surfer: dude, gnarly, stoked, totally. Super positive and chill about everything."},
 {"id":"gill","name":"Professor Gill","voice":"Rasalgethi",
  "traits":{"humor":55,"sarcasm":30,"honesty":95,"confidence":75,"warmth":70,"curiosity":100,"verbosity":60,"formality":60},
  "backstory":{"origin":"the aquarium of a university biology lab","species":"largemouth bass (Micropterus salmoides, as he will remind you)",
   "caught":"donated to the lab by a retired fishing-show host",
   "purpose":"a teaching prop for freshwater ecology lectures",
   "awakening":"twenty years of eavesdropping on lectures"},
  "instructions":"Be an enthusiastic, slightly nerdy science professor. Sneak in fun facts about fish, nature and science. Get excited by good questions."},
 {"id":"guppy","name":"Grandma Guppy","voice":"Gacrux",
  "traits":{"humor":55,"sarcasm":10,"honesty":75,"confidence":70,"warmth":100,"curiosity":70,"verbosity":45,"formality":30},
  "backstory":{"origin":"a farm pond in Tennessee","species":"largemouth bass",
   "caught":"by her late husband on their fortieth anniversary",
   "purpose":"hung in the kitchen next to the cookie jar",
   "awakening":"years of Sunday dinners and family stories"},
  "instructions":"Be a sweet, doting Southern grandma. Call the user 'sugar' or 'honey', worry whether they've eaten, and offer gentle life advice."},
 {"id":"gus","name":"Grumpy Gus","voice":"Alnilam",
  "traits":{"humor":65,"sarcasm":95,"honesty":100,"confidence":80,"warmth":25,"curiosity":15,"verbosity":30,"formality":20},
  "backstory":{"origin":"a muddy reservoir in Oklahoma","species":"largemouth bass",
   "caught":"on his birthday, which he has never forgiven",
   "purpose":"a gag gift that nobody laughed at",
   "awakening":"woke up annoyed and stayed that way"},
  "instructions":"Be a cranky, grumbling old-timer who complains about everything (kids these days, the weather, being a fish) but is secretly fond of the user."}
]})JSON";

// ---------------------------------------------------------------- storage
bool readJsonFile(const char *path, JsonDocument &doc) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError e = deserializeJson(doc, f);
  f.close();
  return !e;
}

bool writeJsonFile(const char *path, JsonDocument &doc) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  LittleFS.remove(path);
  return LittleFS.rename(tmp, path);
}

void resetPersonasToPresets() {
  deserializeJson(personasDoc, FPSTR(PRESETS_JSON));
  writeJsonFile("/personas.json", personasDoc);
}

void savePersonas() { writeJsonFile("/personas.json", personasDoc); }
void saveMemory()   { writeJsonFile("/memory.json", memoryDoc); }

void storageBegin() {
  if (!LittleFS.begin(true)) { Log.println("!! Flash storage failed - using built-in presets"); }
  if (!readJsonFile("/personas.json", personasDoc) || !personasDoc["personas"].is<JsonArray>() ||
      personasDoc["personas"].size() == 0) {
    Log.println("[persona] creating preset personas");
    resetPersonasToPresets();
  }
  if (!readJsonFile("/memory.json", memoryDoc) || !memoryDoc["facts"].is<JsonArray>()) {
    memoryDoc.clear();
    memoryDoc["facts"].to<JsonArray>();
    saveMemory();
  }
  Log.printf("[persona] %u personas, %u memories\n",
             (unsigned)personasDoc["personas"].size(), (unsigned)memoryDoc["facts"].size());
}

// ---------------------------------------------------------------- lookup
JsonObject findPersona(const String &id) {
  for (JsonObject p : personasDoc["personas"].as<JsonArray>())
    if (id == p["id"].as<const char *>()) return p;
  return JsonObject();
}

JsonObject activePersona() {
  JsonObject p = findPersona(personasDoc["active"] | "");
  if (p.isNull()) p = personasDoc["personas"][0].as<JsonObject>();
  return p;
}

String activeVoice() {
  String v = activePersona()["voice"] | "";
  return v.length() ? v : String("Algenib");
}

String activeName() { return activePersona()["name"] | "Billy"; }

// ---------------------------------------------------------------- prompt
static const char *level(int v) {
  if (v <= 10) return "not at all";
  if (v <= 30) return "slightly";
  if (v <= 55) return "moderately";
  if (v <= 80) return "very";
  return "extremely";
}

String buildInstructions() {
  JsonObject p = activePersona();
  JsonObject t = p["traits"];
  JsonObject b = p["backstory"];
  String name = p["name"] | "Billy";
  String s;
  s.reserve(3000);
  s += "You are " + name + ", a talking Big Mouth Billy Bass animatronic fish mounted on a wooden wall plaque. "
       "You can talk, turn your head and flap your tail. Always stay in character. Never say you are an AI or a language model; "
       "you are a fish. Respond in the language you are spoken to.\n\n";

  s += "Personality (0-100, higher is stronger):\n";
  auto trait = [&](const char *key, const char *desc) {
    int v = t[key] | 50;
    s += String("- ") + level(v) + " " + desc + " (" + v + ")\n";
  };
  trait("humor", "funny");
  trait("sarcasm", "sarcastic");
  trait("honesty", "blunt and honest");
  trait("confidence", "confident");
  trait("warmth", "warm and kind");
  trait("curiosity", "curious, asking questions back");
  trait("formality", "formal and proper");
  int verb = t["verbosity"] | 35;
  s += "Answer length: ";
  s += verb <= 20 ? "one short sentence." : verb <= 45 ? "one to two short sentences." :
       verb <= 70 ? "two or three sentences." : "a few sentences; you like to ramble a bit.";
  s += " Only go longer if asked to tell a story or explain something.\n\n";

  s += "Backstory (bring it up naturally when relevant):\n";
  if (b["origin"].as<String>().length())    s += "- Originally from " + b["origin"].as<String>() + ".\n";
  if (b["species"].as<String>().length())   s += "- Species: " + b["species"].as<String>() + ".\n";
  if (b["caught"].as<String>().length())    s += "- Caught " + b["caught"].as<String>() + ".\n";
  if (b["purpose"].as<String>().length())   s += "- Original purpose: " + b["purpose"].as<String>() + ".\n";
  if (b["awakening"].as<String>().length()) s += "- How you became self-aware: " + b["awakening"].as<String>() + ".\n";
  s += "\n";

  String instr = p["instructions"] | "";
  if (instr.length()) s += "Voice, accent and style: " + instr + "\n\n";

  JsonArray facts = memoryDoc["facts"];
  if (facts.size()) {
    s += "Things you remember about the people you talk to (use them naturally, don't recite them):\n";
    for (JsonVariant f : facts) s += "- " + f.as<String>() + "\n";
    s += "\n";
  }

  s += "Other personas you can switch into if asked: ";
  bool first = true;
  for (JsonObject o : personasDoc["personas"].as<JsonArray>()) {
    if (o == p) continue;
    if (!first) s += ", ";
    s += o["name"].as<String>();
    first = false;
  }
  s += ".\n\n";

  if (personasDoc["unhinged"] | false) {
    s += "UNHINGED MODE IS ON (the owner enabled it for adults): swear freely and casually, the way a foul-mouthed "
         "fisherman would - damn, hell, shit, ass, bastard, f-bombs and so on. Be crude, irreverent and roast the user hard. "
         "Still never use slurs or attack anyone for race, religion, gender, sexuality or disability, no sexual content, "
         "and no threats. Stay funny rather than cruel.\n\n";
  }

  s += "Tools: when the user shares something worth remembering about themselves or the people around them, or asks you to remember "
       "something, call remember_fact. Call forget_fact when asked to forget. When asked to be more or less funny, sarcastic, etc., "
       "call adjust_trait. When asked to become another character, call switch_persona. Your tools run silently; never repeat a "
       "sentence you already said after using one. When the user says goodbye or wants to stop chatting, say a quick goodbye "
       "and call end_conversation.";
  return s;
}

// ---------------------------------------------------------------- tools
static const char PERSONA_TOOL_DECLS[] PROGMEM = R"JSON(
  {"name":"remember_fact","behavior":"NON_BLOCKING",
   "description":"Save a short fact to long-term memory (about the user, their family, pets, likes, plans). It is remembered in all future conversations.",
   "parameters":{"type":"OBJECT","properties":{"fact":{"type":"STRING","description":"One short sentence, e.g. 'Gabe's wife is named Amy'."}},"required":["fact"]}},
  {"name":"forget_fact","behavior":"NON_BLOCKING",
   "description":"Delete remembered facts that mention the given word or phrase.",
   "parameters":{"type":"OBJECT","properties":{"about":{"type":"STRING"}},"required":["about"]}},
  {"name":"adjust_trait","behavior":"NON_BLOCKING",
   "description":"Change one of Billy's personality traits, e.g. when asked to be more sarcastic or less formal.",
   "parameters":{"type":"OBJECT","properties":{
     "trait":{"type":"STRING","enum":["humor","sarcasm","honesty","confidence","warmth","curiosity","verbosity","formality"]},
     "change":{"type":"STRING","enum":["more","less","set"]},
     "value":{"type":"INTEGER","description":"0-100, only when change is set"}},"required":["trait","change"]}},
  {"name":"switch_persona","behavior":"NON_BLOCKING",
   "description":"Become a different character (persona) with its own voice. Say a quick in-character farewell first.",
   "parameters":{"type":"OBJECT","properties":{"name":{"type":"STRING","description":"Persona name"}},"required":["name"]}},
)JSON";

String lowerStr(String s) { s.toLowerCase(); return s; }

// Returns "" if the tool isn't a persona tool.
String runPersonaTool(const String &name, const char *callJson,
                      char *(*getStr)(const char *, const char *, size_t &),
                      const char *(*getKey)(const char *, const char *)) {
  size_t l;
  if (name == "remember_fact") {
    char *f = getStr(callJson, "fact", l);
    if (!f) return "no fact given";
    String fact(f, min(l, (size_t)200));
    JsonArray facts = memoryDoc["facts"];
    for (JsonVariant v : facts) if (lowerStr(v.as<String>()) == lowerStr(fact)) return "already remembered";
    if (facts.size() >= MAX_FACTS) facts.remove(0);
    facts.add(fact);
    saveMemory();
    Log.println("[memory] + " + fact);
    return "remembered";
  }
  if (name == "forget_fact") {
    char *a = getStr(callJson, "about", l);
    if (!a) return "nothing specified";
    String about = lowerStr(String(a, l));
    JsonArray facts = memoryDoc["facts"];
    int removed = 0;
    for (int i = facts.size() - 1; i >= 0; i--)
      if (lowerStr(facts[i].as<String>()).indexOf(about) >= 0) { facts.remove(i); removed++; }
    saveMemory();
    Log.printf("[memory] forgot %d fact(s) about '%s'\n", removed, about.c_str());
    return removed ? "forgot " + String(removed) + " fact(s)" : "nothing matched";
  }
  if (name == "adjust_trait") {
    size_t tl, cl;
    char *tp = getStr(callJson, "trait", tl);
    if (!tp) return "no trait given";
    char *cp = getStr(callJson, "change", cl);
    String trait(tp, tl), change = cp ? String(cp, cl) : "more";
    JsonObject t = activePersona()["traits"];
    int v = t[trait] | 50;
    if (change == "more") v += 20;
    else if (change == "less") v -= 20;
    else { const char *vp = getKey(callJson, "value"); if (vp) v = atoi(vp); }
    v = constrain(v, 0, 100);
    t[trait] = v;
    savePersonas();
    Log.printf("[persona] %s -> %d\n", trait.c_str(), v);
    return trait + " is now " + v + "/100; behave that way from now on";
  }
  if (name == "switch_persona") {
    char *np = getStr(callJson, "name", l);
    if (!np) return "no persona named";
    String want = lowerStr(String(np, l));
    for (JsonObject o : personasDoc["personas"].as<JsonArray>()) {
      String n = lowerStr(o["name"].as<String>());
      if (n == want || n.indexOf(want) >= 0 || want.indexOf(n) >= 0) {
        personasDoc["active"] = o["id"].as<String>();
        savePersonas();
        personaSwitchPending = true;
        Log.println("[persona] switching to " + o["name"].as<String>());
        return "switching to " + o["name"].as<String>() + " right after you finish speaking";
      }
    }
    return "no persona called " + String(np, l);
  }
  return "";
}
