package com.gaurav.avnc.model

import com.gaurav.avnc.model.db.ProfileSecrets
import javax.crypto.AEADBadTagException
import javax.crypto.KeyGenerator
import org.junit.Assert.*
import org.junit.Test

class ProfileSecurityTest {
    @Test fun credentialsRoundTripAndAuthenticate() {
        val key = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
        val secrets = ProfileSecrets { key }
        val text = "테스트\nfixture"
        val ciphertext = secrets.encrypt(text)
        assertEquals(text, secrets.decrypt(ciphertext))
        assertFalse(ciphertext.contentEquals(secrets.encrypt(text)))
        val otherKey = KeyGenerator.getInstance("AES").generateKey()
        assertThrows(AEADBadTagException::class.java) { ProfileSecrets { otherKey }.decrypt(ciphertext) }
        ciphertext[ciphertext.lastIndex] = (ciphertext.last().toInt() xor 1).toByte()
        assertThrows(AEADBadTagException::class.java) { secrets.decrypt(ciphertext) }
    }

    @Test fun readingNeverCreatesAKey() {
        val key = KeyGenerator.getInstance("AES").generateKey()
        val cipher = ProfileSecrets { key }.encrypt("fixture")
        val unavailable = ProfileSecrets { create ->
            assertFalse(create)
            throw IllegalStateException("Key unavailable")
        }
        assertEquals("", unavailable.decrypt(unavailable.encrypt("")))
        assertThrows(IllegalStateException::class.java) { unavailable.decrypt(cipher) }
    }

    @Test fun permissionsFollowTheConnectionRoute() {
        val server = ServerProfile(host = "desktop.example", port = 5901)
        assertEquals(server.endpointKey(), server.copy(ID = 32, name = "Renamed", port = 1).endpointKey())
        assertNotEquals(server.endpointKey(), server.copy(host = "lab.example").endpointKey())
        assertNotEquals(server.endpointKey(), server.copy(port = 5902).endpointKey())
        val relay = server.copy(useRepeater = true, idOnRepeater = 1)
        assertNotEquals(relay.endpointKey(), relay.copy(idOnRepeater = 2).endpointKey())
        val ssh = server.copy(channelType = ServerProfile.CHANNEL_SSH_TUNNEL, sshHost = "gateway.example")
        assertNotEquals(server.endpointKey(), ssh.endpointKey())
        assertNotEquals(ssh.endpointKey(), ssh.copy(sshPort = 2222).endpointKey())
        assertNotEquals(ssh.endpointKey(), ssh.copy(sshUsername = "other").endpointKey())
    }
}
