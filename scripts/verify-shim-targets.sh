check() { # lib symbol
  if readelf --dyn-syms -W "devlib/$1" 2>/dev/null | grep -q " $2\$\| $2 "; then
    echo "  OK      $2"
  else
    echo "  MISSING $2   (expected in $1)"
  fi
}
echo "libui targets:"
check libui.so '_ZN7android13GraphicBufferC1EjjijjjP13native_handleb'
check libui.so '_ZN7android13GraphicBuffer4lockEjPPvPiS3_'
check libui.so '_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPiS9_'
echo "libdpframework targets:"
check libdpframework.so '_ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure'
check libdpframework.so '_ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure'
check libdpframework.so '_ZN11DpIspStream11startStreamEP7timeval'
check libdpframework.so '_ZN12DpBlitStream10invalidateEP7timeval'
