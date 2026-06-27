function appName()
{
    return installer.value("Name")
}

function appExecutableFileName()
{
    if (runningOnWindows()) {
        return appName() + ".exe";
    } else {
        return appName();
    }
}

function runningOnWindows()
{
    return (systemInfo.kernelType === "winnt");
}

function runningOnMacOS()
{
    return (systemInfo.kernelType === "darwin");
}

function runningOnLinux()
{
    return (systemInfo.kernelType === "linux");
}

function Component()
{
    installer.finishButtonClicked.connect(this, Component.prototype.installationFinished);
}

Component.prototype.createOperations = function()
{
    component.createOperations();

    if (runningOnWindows()) {
        component.addOperation("CreateShortcut", "@TargetDir@/" + appExecutableFileName(),
                               QDesktopServices.storageLocation(QDesktopServices.DesktopLocation) + "/" + appName() + ".lnk",
                               "workingDirectory=@TargetDir@", "iconPath=@TargetDir@\\" + appExecutableFileName(), "iconId=0");

        component.addElevatedOperation("CreateShortcut", "@TargetDir@/" + appExecutableFileName(),
                                       installer.value("AllUsersStartMenuProgramsPath") + "/" + appName() + ".lnk",
                                       "workingDirectory=@TargetDir@", "iconPath=@TargetDir@\\" + appExecutableFileName(), "iconId=0");
    }
}

Component.prototype.installationFinished = function()
{
    var command = "";
    var args = [];

    if ((installer.status !== QInstaller.Success) || (!installer.isInstaller() && !installer.isUpdater())) {
        return;
    }

    if (runningOnWindows()) {
        command = "@TargetDir@/" + appExecutableFileName()
    } else if (runningOnMacOS()) {
        command = "/Applications/" + appName() + ".app/Contents/MacOS/" + appName();
    } else if (runningOnLinux()) {
        command = "@TargetDir@/client/" + appName();
    }

    if (command.length > 0) {
        installer.executeDetached(command, args, installer.value("TargetDir"));
    }
}
