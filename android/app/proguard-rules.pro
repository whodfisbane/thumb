# apksig uses reflection-free JCA lookups, but keep its public API intact.
-keep class com.android.apksig.** { *; }
-dontwarn com.android.apksig.**
