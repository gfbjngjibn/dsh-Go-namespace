package io.github.ccch1mneyyy.dsh_app.phone

import java.net.InetAddress
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PhoneBridgeContainerTest {
    private fun ip(text: String) = InetAddress.getByName(text)

    @Test fun theGatewaySocketServesOnlyTheContainersSubnet() {
        assertTrue(PhoneBridge.fromContainer(ip("10.213.0.2")))
        assertTrue(PhoneBridge.fromContainer(ip("10.213.255.254")))
        // A hotspot client, the LAN, the phone itself and look-alikes are refused.
        assertFalse(PhoneBridge.fromContainer(ip("192.168.43.10")))
        assertFalse(PhoneBridge.fromContainer(ip("10.214.0.2")))
        assertFalse(PhoneBridge.fromContainer(ip("127.0.0.1")))
        assertFalse(PhoneBridge.fromContainer(ip("::1")))
        assertFalse(PhoneBridge.fromContainer(null))
    }
}
