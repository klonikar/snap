// pseudo-ibss.click

// This configuration contains a configuration for running 
// in 802.11 "pseudo-ibss" (or psuedo ad-hoc) mode, where
// no management packets are sent.
// It creates an interface using FromHost called "safe"
// This configuration assumes that you have a network interface named
// ath0 with the mac address located in the AddressInfo

// Run it at user level with
// 'click pseudo-ibss.click'

// Run it in the Linux kernel with
// 'click-install pseudo-ibss.click'
// Messages are printed to the system log (run 'dmesg' to see them, or look
// in /var/log/messages), and to the file '/click/messages'.

AddressInfo(safe_addr 6.70.151.40/8 00:05:4E:46:97:28);
winfo :: WirelessInfo(BSSID 00:00:00:00:00:00);

FromHost(safe, safe_addr, ETHER safe_addr)
-> q :: Queue()
-> encap :: WifiEncap(0x0, WIRELESS_INFO winfo)
-> set_power :: SetTXPower(63)
-> set_rate :: SetTXRate(2)
-> extra_encap :: ExtraEncap()
-> to_dev :: ToDevice(ath0);



from_dev :: FromDevice(ath0)
-> prism2_decap :: Prism2Decap()
-> extra_decap :: ExtraDecap()
-> phyerr_filter :: FilterPhyErr()
-> tx_filter :: FilterTX()
-> dupe :: WifiDupeFilter(WINDOW 20)
-> wifi_cl :: Classifier(0/08%0c 1/00%03) //nods data
-> WifiDecap()
-> SetPacketType(HOST)
-> ToHost(safe);

