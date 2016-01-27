"use strict";

Components.utils.import("resource://gre/modules/XPCOMUtils.jsm");

/* global Downloads: false */
Components.utils.import("resource://gre/modules/Downloads.jsm");

/* global OS: false */
Components.utils.import("resource://gre/modules/osfile.jsm");

/* global DownloadIntegration: false */
XPCOMUtils.defineLazyModuleGetter(this, "DownloadIntegration",
                                  "resource://gre/modules/DownloadIntegration.jsm");

/* global ExtensionUtils */
Cu.import("resource://gre/modules/ExtensionUtils.jsm");
const {
  EventManager,
  ignoreEvent,
} = ExtensionUtils;

let lastError = null;

let currentId = 0;
const downloadsList = {};
const extensionSpecificInfo = new Map();

function DownloadItem(downloadId) {
  this.id = downloadId;
}

const DownloadInfo = {
  fromId(downloadId) {
    const download = downloadsList[downloadId];
    if (!download) {
      throw new Error(`Bad downloadId '${downloadId}'`);
    }
    return download;
  },

  newFromDownload(download, extension) {
    const downloadInfo = downloadsList[++currentId] = { download };
    const item = downloadInfo.downloadItem = new DownloadItem(currentId);
    extensionSpecificInfo.set(item, { extension }); // TODO remove it when the extension is uninstalled
    return downloadInfo;
  },
};

function getDownload(downloadId) {
  return DownloadInfo.fromId(downloadId).download;
}

DownloadItem.fromDownload = (download) => {
  const id = Object.keys(downloadsList).find(
    downloadId => downloadsList[id].download === download
  );

  return downloadsList[id].downloadItem;
};

DownloadItem.prototype = {
  get url() { return getDownload(this.id).source.url; },
  get referrer() { return getDownload(this.id).source.referrer; },
  get localname() { return getDownload(this.id).target.path; },
  get incognito() { return getDownload(this.id).source.isPrivate; },
  get danger() { return "safe"; }, // TODO
  get mime() { return getDownload(this.id).contentType; },
  get startTime() {
    const download = getDownload(this.id);
    return download.startTime && download.startTime.toISOString();
  },
  get endTime() { return null; }, // TODO
  get estimatedEndTime() { return null; }, // TODO
  get state() {
    const download = getDownload(this.id);
    if (download.succeeded) {
      return "complete";
    }
    if (download.stopped) {
      return "interrupted";
    }
    return "in_progress";
  },
  get canResume() {
    const download = getDownload(this.id);
    return download.stopped && download.hasPartialData;
  },
  get error() {
    const download = getDownload(this.id);
    if (!download.stopped || download.succeeded) {
      return null;
    }
    // TODO store this instead of calculating it

    if (download.error) {
      if (download.error.becauseSourceFailed) {
        return "NETWORK_FAILED"; // TODO
      }
      if (download.error.becauseTargetFailed) {
        return "FILE_FAILED"; // TODO
      }
      return "CRASH";
    }
    return "USER_CANCELED";
  },
  get bytesReceived() {
    return getDownload(this.id).currentBytes;
  },
  get totalBytes() {
    const download = getDownload(this.id);
    return download.hasProgress ? download.totalBytes : -1;
  },
  get fileSize() {
    const download = getDownload(this.id);
    return download.succeeded ? download.target.size : -1;
  },
  get exists() {
    return getDownload(this.id).target.exists;
  },

  // extensionSpecificInfo is populated by DownloadInfo.newFromDownload.
  // It's in a separate map because it will need to be persisted separately
  get byExtensionId() {
    return extensionSpecificInfo.get(this).extension.id;
  },
  get byExtensionName() {
    return extensionSpecificInfo.get(this).extension.name;
  },
};

const downloadsFactory = (extension, context) => ({
  downloads: Object.freeze({
    download(chromeOptions, callback) {
      Downloads.getPreferredDownloadsDirectory().then(downloadsDir => {
        const mozOptions = {
          source: chromeOptions.url,
          // TODO use file path to check and concatenate
          target: chromeOptions.target ?
            OS.Path.join(downloadsDir, chromeOptions.target) :
            downloadsDir,
          // TODO do something with conflictAction,
          // TODO do something with saveAs,
        };

        return Downloads.createDownload(mozOptions);
      }).then(download => {
        download.tryToKeepPartialData = true;
        const downloadInfo = DownloadInfo.newFromDownload(download, extension);
        downloadInfo.currentPromise = download.start();
        return downloadInfo.downloadItem.id;
      }).catch(e => {
        // TODO put something in runtime.lastError
        lastError = e;
        return undefined;
      }).then(callback);
    },

    search(query, callback) {
      throw new Error("not implemented");
    },

    pause(downloadId, callback) {
      Promise.resolve().then(() => getDownload(downloadId))
      .then(download => download.cancel())
      .catch(e => {
        lastError = e;
      }).then(callback);
    },

    resume(downloadId, callback) {
      Promise.resolve().then(() => DownloadInfo.fromId(downloadId))
      .then(downloadInfo => {
        downloadInfo.promise = downloadInfo.download.start();
      }).catch(e => {
        lastError = e;
      }).then(callback);
    },

    cancel(downloadId, callback) {
      Promise.resolve().then(() => getDownload(downloadId))
      .then(download => {
        const returnPromise = download.finalize();
        delete downloadsList[downloadId];
        return returnPromise;
      }).catch(e => {
        lastError = e;
      }).then(callback);
    },

    getFileIcon(downloadId, options, callback) {
      throw new Error("not implemented");
    },

    show(downloadId) {
      Promise.resolve().then(() => getDownload(downloadId))
      .then(download => download.showContainingDirectory())
      .catch(e => {
        lastError = e;
      });
    },

    showDefaultFolder() {
      DownloadIntegration.showContainingDirectory(
        Downloads.getPreferredDownloadsDirectory()
      );
    },

    erase(query, callback) {
      throw new Error("not implemented");
    },

    removeFile(downloadId, callback) {
      Promise.resolve().then(() => getDownload(downloadId))
      .then(download => {
        if (!download.succeeded) {
          throw new Error(`The download '${downloadId}' isn't complete yet`);
        }

        return OS.File.remove(download.target.path, { ignoreAbsent: true });
      }).catch(e => {
        lastError = e;
      }).then(callback);
    },

    acceptDanger(downloadId, callback) {
      throw new Error("not implemented");
    },

    drag(downloadId) {
      throw new Error("not implemented");
    },

    setShelfEnabled(enabled) {
      // useless in Firefox
    },

    onCreated: new EventManager(context, "downloads.onCreated", fire => {
      return Downloads.getList(Downloads.ALL).then(list => {
        let isAdded = false;
        const view = {
          onDownloadAdded(download) {
            // TODO create downloadItem from download
            const downloadItem = DownloadItem.fromDownload(download);
            if (isAdded) {
              // we want to emit only the _new_ items, not the existing ones.
              fire(downloadItem);
            }
          },
        };

        list.addView(view).then(() => { isAdded = true; });
        return () => list.removeView(view);
      });
    }).api(),
  }),
});

const downloadsOpenFactory = (extension, context) => ({
  downloads: Object.freeze({
    open(downloadId) {
      Promise.resolve().then(() => getDownload(downloadId))
      .then(download => {
        if (!download.succeeded) {
          throw new Error(`The download '${downloadId}' isn't complete yet`);
        }
        return download.launch();
      }).catch(e => {
        lastError = e;
      });
    },
  }),
});

extensions.registerPrivilegedAPI("downloads", downloadsFactory);
extensions.registerPrivilegedAPI(["downloads", "downloads.open"], downloadsOpenFactory);
