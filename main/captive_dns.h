// Tiny DNS server for the setup hotspot: answers every A query with the
// hotspot address so phones/laptops open the captive portal (WiFiManager did
// the same with DNSServer).
#pragma once

namespace captive_dns {

void Start();

}  // namespace captive_dns
