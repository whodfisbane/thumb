package dev.thumb.app.core

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import com.android.apksig.ApkSigner
import java.io.File
import java.math.BigInteger
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.cert.X509Certificate
import java.util.Calendar
import javax.security.auth.x500.X500Principal

/**
 * Signs patched APKs with this phone's THUMB key, created on first use in
 * Android Keystore (hardware-backed where available; it never leaves the
 * device). All apps THUMB patches on this phone share this key, so they can
 * be updated by re-patching.
 */
object Signer {
    private const val ALIAS = "thumb-signing-key-v2"
    private const val OLD_ALIAS = "thumb-signing-key" // SHA-256-only, could not sign for old minSdk

    private fun key(): Pair<PrivateKey, X509Certificate> {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (ks.containsAlias(OLD_ALIAS)) ks.deleteEntry(OLD_ALIAS)
        if (!ks.containsAlias(ALIAS)) {
            val start = Calendar.getInstance()
            val end = Calendar.getInstance().apply { add(Calendar.YEAR, 50) }
            val spec = KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_SIGN)
                .setKeySize(2048)
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512, KeyProperties.DIGEST_SHA1)
                .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
                .setCertificateSubject(X500Principal("CN=THUMB on-device signing key"))
                .setCertificateSerialNumber(BigInteger.ONE)
                .setCertificateNotBefore(start.time)
                .setCertificateNotAfter(end.time)
                .build()
            KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore").apply { initialize(spec) }.generateKeyPair()
        }
        val entry = ks.getEntry(ALIAS, null) as KeyStore.PrivateKeyEntry
        return entry.privateKey to (entry.certificate as X509Certificate)
    }

    fun sign(unsigned: File, signed: File) {
        val (privateKey, cert) = key()
        val config = ApkSigner.SignerConfig.Builder("THUMB", privateKey, listOf(cert)).build()
        ApkSigner.Builder(listOf(config))
            .setInputApk(unsigned)
            .setOutputApk(signed)
            // Patched apps only run on 64-bit phones (Android 5+); signing for 7+
            // lets every scheme use SHA-256 even if the app claims minSdk 3.
            .setMinSdkVersion(24)
            .setV1SigningEnabled(true) // old apps may target pre-Nougat installers
            .setV2SigningEnabled(true)
            .setV3SigningEnabled(true)
            .build()
            .sign()
    }

    /** SHA-256 of our signing certificate, to tell THUMB-patched apps apart. */
    fun certificateDigest(): ByteArray =
        java.security.MessageDigest.getInstance("SHA-256").digest(key().second.encoded)
}
