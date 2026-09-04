import nuke

# Register DLSS 5 Native Op to Nuke Menu
toolbar = nuke.menu("Nodes")
dlss_menu = toolbar.addMenu("DLSS5", icon="DLSS5.png")
dlss_menu.addCommand("DLSS 5 Live", "nuke.createNode('DLSS5Live')", icon="DLSS5.png")
