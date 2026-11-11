package com.droiddeck.launcher.core

import android.content.Context
import android.os.Build
import com.droiddeck.launcher.R
import com.droiddeck.launcher.gpu.SystemVulkanDriver
import java.io.File

object DeviceSupport {
    private val adrenoGpu by lazy {
        File("/sys/class/kgsl/kgsl-3d0").exists() || File("/vendor/lib64/hw/vulkan.adreno.so").exists()
    }

    fun adreno(): Boolean = adrenoGpu

    fun graphicsAvailable(): Boolean = graphicsAvailable(adreno(), SystemVulkanDriver.isAvailable())

    internal fun graphicsAvailable(adreno: Boolean, powerVrReady: Boolean): Boolean = adreno || powerVrReady

    fun vkBridge(context: Context): Boolean =
        !ownMaliPath(context) && bridgeUsable() && !com.droiddeck.launcher.session.VenusComponent.wanted(context) &&
            com.droiddeck.launcher.session.SessionPrefs.vkBridge(context)

    fun mali(): Boolean = isMali

    private val isMali by lazy {
        !adreno() && (File("/dev/mali0").exists() ||
            com.droiddeck.launcher.gpu.GpuInfo.detect().family.name.startsWith("MALI"))
    }

    fun bridgeUsable(): Boolean = !adreno() && !SystemVulkanDriver.isAvailable() && File("/dev/mali0").exists()

    fun openDriverActive(context: Context): Boolean {
        if (adreno() || ownMaliPath(context)) return false
        if (com.droiddeck.launcher.session.VenusComponent.wanted(context)) return false
        if (bridgeUsable() && com.droiddeck.launcher.session.SessionPrefs.vkBridge(context)) return false
        val linux = com.droiddeck.launcher.session.SessionPrefs.linuxDriver(context).ifEmpty { return false }
        return com.droiddeck.launcher.gpu.DriverPairs.isNonAdreno(com.droiddeck.launcher.gpu.DriverBundle.containing(context, linux, true)?.id)
    }

    // [mali-g2] supported(): a device this build forwards Vulkan for with Vortek can draw too.
    fun canDraw(context: Context): Boolean = supported(context) || graphicsAvailable() || openDriverActive(context) ||
        vkBridge(context) || com.droiddeck.launcher.session.VenusComponent.wanted(context)

    /** [mali-g2] Adreno, or a device whose vendor Vulkan driver this build forwards to the guest with Vortek. */
    fun supported(context: Context): Boolean =
        adreno() || com.droiddeck.launcher.session.VortekServerComponent.available(context)

    /**
     * [mali-g2] This build's own Mali path (Vortek forwarding, libhybris for games) runs the sessions on this device.
     * Upstream's Mali routes (the Mali bridge, Venus, open driver pairs and what Auto switches on in their place)
     * are then off, whatever their settings say. Session pref gpuPath=upstream hands the device to them instead.
     */
    fun ownMaliPath(context: Context): Boolean =
        com.droiddeck.launcher.session.VortekServerComponent.available(context) &&
            com.droiddeck.launcher.session.SessionPrefs.gpuPath(context) != com.droiddeck.launcher.session.HybrisGpu.UPSTREAM

    /** [mali-g2] No Adreno, but a vendor Vulkan HAL (Mali): the compositor must use the system driver. */
    fun vendorVulkanOnly(): Boolean =
        !adreno() && com.droiddeck.launcher.session.VortekServerComponent.halDriver() != null

    fun gpuName(context: Context): String {
        val soc = if (Build.VERSION.SDK_INT >= 31) Build.SOC_MODEL.takeIf { it.isNotBlank() && it != Build.UNKNOWN } else null
        return soc?.let { "$it (${Build.HARDWARE})" } ?: Build.HARDWARE.ifBlank { context.getString(R.string.gpu_this_gpu) }
    }

    fun lowLatencyWifiSafe(): Boolean = Build.VERSION.SDK_INT < 31 || Build.SOC_MODEL != "CQ8725S"
}
