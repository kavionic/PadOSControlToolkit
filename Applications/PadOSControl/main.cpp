// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include <QApplication>
#include <QStyleFactory>
#include <PadOSControl/Widgets/ControlWindow.h>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName("PadOSControl");
    application.setApplicationDisplayName("PadOS Control");
    application.setApplicationVersion("0.1");
    application.setOrganizationName("SkauenInnovation");
    application.setOrganizationDomain("kavionic.com");
    application.setStyle(QStyleFactory::create("Fusion"));

    ControlWindow window;
    window.Start();
    window.show();
    return application.exec();
}
