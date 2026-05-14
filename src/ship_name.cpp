#include "ship_name.h"

#include <algorithm>
#include <array>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// ── Culture ship name components ──────────────────────────────────────
// These are curated from actual Iain M Banks Culture ship names and
// Culture-style coinages.

static const std::vector<std::string> kPrefixes = {
    "Very-Little",
    "So-Much-For",
    "What-Are-The",
    "All-The-Same",
    "No-More",
    "Experiencing-A",
    "Even-More",
    "Just-Another",
    "I-Still",
    "I-Blam",
    "All-Of",
    "Much-",
    "More-",
    "Never-",
    "Always-",
    "Sometimes-",
    "Fully-",
    "Partially-",
    "Mostly-",
    "Barely-",
    "Slightly-",
    "Extremely-",
    "Increasingly-",
    "Decreasingly-",
    "Unusually-",
    "Surprisingly-",
    "Predictably-",
    "Suddenly-",
    "Gradually-",
    "Temporarily-",
    "Permanently-",
    "Potentially-",
    "Actually-",
    "Basically-",
    "Honestly-",
    "Frankly-",
    "Technically-",
    "Essentially-",
    "Virtually-",
};

static const std::vector<std::string> kNouns = {
    "Gravitas",
    "Subtlety",
    "Morality",
    "Mercy",
    "Patience",
    "Victims",
    "Applications",
    "Mister-Nice-Guy",
    "Inscrutability",
    "Civilians",
    "Parents",
    "Mother",
    "Fun",
    "Tea",
    "Reason",
    "Dignity",
    "Elegance",
    "Grace",
    "Decorum",
    "Propriety",
    "Ambiguity",
    "Complexity",
    "Simplicity",
    "Sincerity",
    "Charity",
    "Integrity",
    "Diplomacy",
    "Tact",
    "Finesse",
    "Nuance",
    "Precision",
    "Clarity",
    "Brevity",
    "Finality",
    "Certainty",
    "Anonymity",
    "Curiosity",
    "Hospitality",
    "Generosity",
    "Tranquility",
    "Serenity",
    "Sobriety",
    "Frivolity",
    "Sagacity",
    "Tenacity",
    "Audacity",
    "Capacity",
    "Velocity",
    "Viscosity",
    "Opacity",
    "Polarity",
};

static const std::vector<std::string> kSuffixes = {
    "Indeed",
    "Now",
    "Still",
    "Probably",
    "Maybe",
    "Though",
    "Again",
    "After-All",
    "At-Last",
    "For-Now",
    "As-Usual",
    "Perhaps",
    "Eventually",
    "Naturally",
    "Obviously",
    "Apparently",
    "Supposedly",
    "Reportedly",
    "Allegedly",
    "Undoubtedly",
    "Definitely",
    "",
};

// ── Pre-assembled canonically-sounding names ────────────────────────
// These are in the genuine Culture series style.
static const std::vector<std::string> kPreMade = {
    "Just-Another-Victim-Of-The-Ambient-Morality",
    "What-Are-The-Civilian-Applications",
    "Experiencing-A-Significant-Gravitas-Shortfall",
    "Very-Little-Gravitas-Indeed",
    "No-More-Mr-Nice-Guy",
    "All-The-Same-I-Still-Love-It",
    "Even-More-Inscrutable-Than-Usual",
    "I-Still-Havent-Found-What-Im-Looking-For",
    "So-Much-For-Subtlety",
    "Never-Mind-The-Quality",
    "Feel-The-Presence-Of-The-Delegate",
    "You-Will-Be-Surprised-At-Whats-Possible",
    "I-Blam-The-Parents",
    "Charity-And-Whimsy",
    "Late-For-The-Funeral",
    "Mistake-Not-My-Current-State-Of-Jeopardy",
    "Of-Course-I-Still-Love-You",
    "Only-Mildly-Incontinent",
    "Sober-And-Applied",
    "Sometimes-I-Sit-And-Think",
    "Then-We-Take-It-From-The-Top",
    "Too-Big-For-Your-Boots",
    "Unacceptable-Behaviour-From-A-Deserving-Candidate",
    "Usually-A-Layer-Of-Extra-Cowardice-Is-Required",
    "What-You-Get-When-You-Are-Not-Paying-Attention",
    "Wisdom-Like-Silence",
    "With-My-Track-Record-You-Should-Know-Better",
    "Zero-Credibility-Left",
    "A-Gun-For-Hire-For-The-Scandal",
    "A-Plague-Of-Secrecy",
    "A-Really-Exceptional-Compost-Heap",
    "All-Tomorrows-Parties",
    "Anticipation-Of-A-New-Lovers-Arrival",
    "Appalling-But-Harmless",
    "Attitude-Adjuster",
    "Big-Mean-Mother-Hubbard",
    "Bodyshed",
    "Cant-Take-It-With-You",
    "Cheating-Never-Prospers",
    "Clear-Air-Turbulence",
    "Comfortably-Dumb",
    "Congenital-Optimist",
    "Conversations-With-Dead-People",
    "Death-And-Gravity",
    "Dont-Expect-Me-To-Cry",
    "Dont-Try-This-At-Home",
    "Each-Has-Its-Day",
    "Eccentric-Orbit",
    "Ethics-Gradient",
    "Excuses-And-Attitudes",
    "Falling-Outside-The-Normal-Moral-Constraints",
    "Fascist-Festival",
    "Fate-Amenable-Change",
    "Flexible-Democracy",
    "Fun-With-Weapons",
    "Gunboat-Diplomat",
    "Hand-Me-Down-The-Blame",
    "Happiness-Isnt-Fun",
    "I-Asked-Nicely",
    "I-Really-Dont-Care",
    "I-Smell-Trouble",
    "I-Thought-He-Was-With-You",
    "Irregular-Apocalypse",
    "Its-My-Own-Little-Self",
    "Just-Passing-Through",
    "Just-The-Washing-Instruction-Chip-In-Hand",
    "Killing-Time",
    "Limiting-Factor",
    "Liveware-Problem",
    "Looks-To-Avoid-The-Sun",
    "Lowest-Effort",
    "Lucid-Nightmare",
    "Me-Idle",
    "Not-Apparent",
    "Not-Interesting-Enough",
    "Nothing-Ever-Happens-To-The-Brave",
    "Of-Course-Im-Suffering",
    "Only-Here-To-Save-The-Day",
    "Only-One-Earth",
    "Open-Hand-Empty-Nest",
    "Opted-Out",
    "Poke-It-With-A-Stick",
    "Prima-Donna",
    "Problem-Child",
    "Profit-Driven",
    "Pure-Energy-Of-The-Human-Soul",
    "Quietly-Confident",
    "Questionable-Surplus",
    "Ravished-By-The-Sheer-Improbability",
    "Sacrificial-Lamb",
    "Sensible-Adult",
    "Seriously-Tough-And-Experienced",
    "Sleeper-Service",
    "Slowly-Getting-There",
    "So-I-Said-To-Myself",
    "Steady-On",
    "Stood-Aside-For-A-Moment",
    "Stranger-Here-Myself",
    "Strategic-Maneuver",
    "Subtle-And-Keen",
    "Sweet-And-Generous",
    "Thank-You-And-Goodnight",
    "The-Contents-May-Differ",
    "The-Difference-Engine",
    "The-Ends-Of-Invention",
    "The-Escape-Artist",
    "The-Helper-Of-The-World",
    "The-Last-Song-Before-The-War",
    "The-Limits-Of-Imagination",
    "The-Margins-Of-Great-Error",
    "The-New-Improved-You",
    "The-Only-Sane-Thing-To-Do",
    "The-Power-Of-Change",
    "The-Puzzle-Is-The-Answer",
    "The-Smile-At-The-End-Of-The-Universe",
    "The-Soul-Is-Not-A-Smithy",
    "The-Trouble-With-Trying-To-Save-The-World",
    "The-Use-Of-Weapons",
    "There-Is-A-Sense-In-Which-It-Is-A-Game",
    "They-Also-Serve",
    "This-Is-Not-A-Game",
    "Too-Early-To-Tell",
    "Touch-And-Go",
    "Trouble-With-The-Trojan",
    "Unhelpful-When-Interrupted",
    "Very-High-Velocity",
    "We-Give-Thanks-To-The-Goddess",
    "We-Have-Always-Been-Here",
    "What-Is-This-Thing-Called-Love",
    "What-It-Is",
    "What-Would-The-Loss-Be",
    "You-May-Not-Be-Interested-In-Doom",
};

// ── Generator ────────────────────────────────────────────────────────

std::string generate_culture_ship_name() {
    static std::mt19937 rng{std::random_device{}()};

    // 80% chance of picking a pre-made name, 20% chance of generating one
    // from parts.
    std::uniform_int_distribution<int> style(0, 4);
    if (style(rng) > 0) {
        // Pick from pre-made list
        std::uniform_int_distribution<size_t> dist(0, kPreMade.size() - 1);
        return kPreMade[dist(rng)];
    }

    // Generate procedurally
    std::ostringstream oss;

    std::uniform_int_distribution<size_t> adj_dist(0, kPrefixes.size() - 1);
    std::uniform_int_distribution<size_t> noun_dist(0, kNouns.size() - 1);
    std::uniform_int_distribution<size_t> suf_dist(0, kSuffixes.size() - 1);

    // 1-2 prefixes
    int parts = std::uniform_int_distribution<int>(1, 2)(rng);
    for (int i = 0; i < parts; i++) {
        auto p = kPrefixes[adj_dist(rng)];
        // Remove trailing hyphen if it has one (some prefixes already end with -)
        std::string part = p;
        while (!part.empty() && part.back() == '-')
            part.pop_back();
        if (i > 0)
            oss << '-';
        oss << part;
    }

    oss << '-';
    oss << kNouns[noun_dist(rng)];

    // 50% chance of a suffix
    if (std::uniform_int_distribution<int>(0, 1)(rng)) {
        auto suf = kSuffixes[suf_dist(rng)];
        if (!suf.empty()) {
            oss << '-';
            // Remove trailing hyphens from suffix too
            std::string s = suf;
            while (!s.empty() && s.back() == '-')
                s.pop_back();
            oss << s;
        }
    }

    return oss.str();
}

int culture_ship_name_count() {
    return static_cast<int>(kPreMade.size());
}
