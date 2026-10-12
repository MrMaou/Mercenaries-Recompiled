// Item and location tables for the in-game Archipelago client. The data itself
// is generated from the apworld into ap_data.inc (see mercs-ap
// tools/export_recomp_plugin.py); this header only defines its shape.
#pragma once
#include <cstdint>

namespace apdata {

enum class ItemKind { ShopUnlock, Coupon, Money, Reputation, Intel };

struct Item {
  int64_t id;
  ItemKind kind;
  const char *name;
  const char *shop_template;  // ShopUnlock: template name without template_support_
  uint64_t groups;            // ShopUnlock: its groups; Coupon: the groups it applies to
  int tag;                    // ShopUnlock: engine tag, used for ordering
  int64_t money;              // Money: amount in dollars
  int intel;                  // Intel: amount
  int intel_suit;             // Intel: 0..3 for clubs..spades, -1 for any chapter
  const char *faction;        // Reputation: allies, china, mafia or sk
};

enum class LocationKind { Card, Mission, Bounty };

struct Location {
  int64_t id;
  LocationKind kind;
  const char *name;
  const char *short_name;
  int suit;                   // Card: 0..3 for clubs..spades
  int rank;                   // Card: 1 (ace)..13; Mission: 1..12
  int min_chapter;            // Card: first chapter it is available
  bool never_missable;        // Card: aces and the tutorial two; always true otherwise
  int64_t hint_mission;       // Card: location id of the mission that hints it, or 0
  const char *faction;        // Mission: allies, china, mafia or sk
  int bounty_type;            // Bounty: index into BOUNTY_TYPES
  int bounty_count;           // Bounty: collected total needed
};

}  // namespace apdata
