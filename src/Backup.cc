#include <config.h>
#include "rsbackup.h"
#include "Conf.h"
#include "Store.h"
#include "Command.h"
#include "IO.h"
#include "Subprocess.h"
#include "Errors.h"
#include <cerrno>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

// Find a backup to link to.
static const Status *getLastBackup(Volume *volume, Device *device) {
  // The Perl version tries to link against the five most recent backups
  // (complete or not) but this is a lot of extra checking for little real
  // benefit, so this version only links against one.
  //
  // Link against the most recent complete backup if possible.
  for(backups_type::reverse_iterator backupsIterator = volume->backups.rbegin();
      backupsIterator != volume->backups.rend();
      ++backupsIterator) {
    const Status &status = *backupsIterator;
    if(status.rc == 0
       && device->name == status.deviceName)
      return &status;
  }
  // If there are no complete backups link against the most recent incomplete
  // one.
  for(backups_type::reverse_iterator backupsIterator = volume->backups.rbegin();
      backupsIterator != volume->backups.rend();
      ++backupsIterator) {
    const Status &status = *backupsIterator;
    if(device->name == status.deviceName)
      return &status;
  }
  // Otherwise there is nothing to link to.
  return NULL;
}

// Backup VOLUME onto DEVICE.
//
// device->store is assumed to be set.
static void backupVolume(Volume *volume, Device *device) {
  Date today = Date::today();
  Host *host = volume->parent;
  if(command.verbose)
    printf("INFO: backup %s:%s to %s\n",
           host->name.c_str(), volume->name.c_str(),
           device->name.c_str());
  // Synthesize filenames
  const std::string backupPath = (device->store->path
                                  + PATH_SEP + host->name
                                  + PATH_SEP + volume->name
                                  + PATH_SEP + today.toString());
  const std::string logPath = (config.logs
                               + PATH_SEP + today.toString()
                               + "-" + device->name
                               + "-" + host->name
                               + "-" + volume->name
                               + ".log");
  if(command.act) {
    // Create backup directory
    makeDirectory(backupPath);
    // Synthesize command
    std::vector<std::string> cmd;
    cmd.push_back("rsync");
    cmd.push_back("--archive");
    cmd.push_back("--sparse");
    cmd.push_back("--numeric-ids");
    cmd.push_back("--compress");
    cmd.push_back("--fuzzy");
    cmd.push_back("--hard-links");
    if(!command.verbose)
      cmd.push_back("--quiet");
    if(!volume->traverse)
      cmd.push_back("--one-file-system");
    // Exclusions
    for(size_t n = 0; n < volume->exclude.size(); ++n)
      cmd.push_back("--exclude=" + volume->exclude[n]);
    const Status *lastBackup = getLastBackup(volume, device);
    if(lastBackup != NULL)
      cmd.push_back("--link-dest=" + lastBackup->backupPath());
    // Source
    cmd.push_back(host->sshPrefix() + volume->path + "/.");
    // Destination
    cmd.push_back(backupPath + "/.");
    // Set up subprocess
    Subprocess sp(cmd);
    int fd = open(logPath.c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if(fd < 0)
      throw IOError("opening " + logPath, errno);
    sp.addChildFD(1, fd, -1);
    sp.addChildFD(2, fd, -1);
    // Make the backup
    int rc = sp.runAndWait(false);
    // Suppress exit status 24 "Partial transfer due to vanished source files"
    if(WIFEXITED(rc) && WEXITSTATUS(rc) == 24)
      rc = 0;
    // Append status information to the logfile
    StdioFile f;
    f.open(logPath, "a");
    if(rc)
      f.writef("ERROR: device=%s error=%#x\n", device->name.c_str(), rc);
    else
      f.writef("OK: device=%s\n", device->name.c_str());
    f.close();
    // Update recorded state
    // TODO we could perhaps share with Conf::readState() here
    Status s;
    s.rc = rc;
    s.date = today;
    s.deviceName = device->name;
    StdioFile input;
    input.open(logPath, "r");
    input.readlines(s.contents);
    s.volume = volume;
    volume->backups.insert(s);
  }
}

// Return true if VOLUME needs a backup on DEVICE
static bool needsBackup(Volume *volume, Device *device) {
  Date today = Date::today();
  for(backups_type::iterator backupsIterator = volume->backups.begin();
      backupsIterator != volume->backups.end();
      ++backupsIterator) {
    const Status &status = *backupsIterator;
    if(status.rc == 0
       && status.date == today
       && status.deviceName == device->name)
      return false;                     // Already backed up
  }
  return true;
}

// Backup VOLUME
static void backupVolume(Volume *volume) {
  Host *host = volume->parent;
  for(devices_type::iterator devicesIterator = config.devices.begin();
      devicesIterator != config.devices.end();
      ++devicesIterator) {
    Device *device = devicesIterator->second;
    if(needsBackup(volume, device)) {
      config.identifyDevices();
      if(device->store)
        backupVolume(volume, device);
      else if(command.verbose) {
        // TODO maybe this is *too* verbose.
        printf("WARNING: cannot backup %s:%s to %s - device not available\n",
               host->name.c_str(), volume->name.c_str(), device->name.c_str());
      }
    }
  }
}

// Backup HOST
static void backupHost(Host *host) {
  // Do a quick check for unavailable hosts
  if(!host->available()) {
    if(command.verbose)
      printf("WARNING: cannot backup %s - not reachable\n",
             host->name.c_str());
    return;
  }
  for(volumes_type::iterator volumesIterator = host->volumes.begin();
      volumesIterator != host->volumes.end();
      ++volumesIterator) {
    Volume *volume = volumesIterator->second;
    if(volume->selected())
      backupVolume(volume);
  }
}

// Backup everything
void makeBackups() {
  // Load up log files
  config.readState();
  for(hosts_type::iterator hostsIterator = config.hosts.begin();
      hostsIterator != config.hosts.end();
      ++hostsIterator) {
    Host *host = hostsIterator->second;
    if(host->selected())
      backupHost(host);
  }
}
