# SPDX-FileCopyrightText: 2021 Nextcloud GmbH and Nextcloud contributors
# SPDX-FileCopyrightText: 2014 ownCloud GmbH
# SPDX-License-Identifier: GPL-2.0-or-later
# copy the python nautilus plugin to build dir as it is going to
# be changed.
if( UNIX AND NOT APPLE )

    # Read base syncstate.py template
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/syncstate.py" SYNCSTATE_CONTENT)

    # 1. Replace Nextcloud with custom branding app name
    string(REPLACE "Nextcloud" "${APPLICATION_SHORTNAME}" SYNCSTATE_APP_CONTENT "${SYNCSTATE_CONTENT}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/syncstate.py" "${SYNCSTATE_APP_CONTENT}")

    # 2. Create nemo plugin by replacing Nautilus -> Nemo (autilus -> emo)
    string(REPLACE "autilus" "emo" SYNCSTATE_NEMO_CONTENT "${SYNCSTATE_APP_CONTENT}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/syncstate_nemo.py" "${SYNCSTATE_NEMO_CONTENT}")

    # 3. Create caja plugin by replacing Nautilus -> Caja (autilus -> aja)
    string(REPLACE "autilus" "aja" SYNCSTATE_CAJA_CONTENT "${SYNCSTATE_APP_CONTENT}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/syncstate_caja.py" "${SYNCSTATE_CAJA_CONTENT}")

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/syncstate.py DESTINATION ${CMAKE_INSTALL_DATADIR}/nautilus-python/extensions RENAME syncstate-${APPLICATION_SHORTNAME}.py)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/syncstate_nemo.py DESTINATION ${CMAKE_INSTALL_DATADIR}/nemo-python/extensions RENAME syncstate-${APPLICATION_SHORTNAME}.py)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/syncstate_caja.py DESTINATION ${CMAKE_INSTALL_DATADIR}/caja-python/extensions RENAME syncstate-${APPLICATION_SHORTNAME}.py)

endif()

