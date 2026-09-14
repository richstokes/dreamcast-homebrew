// UTC stays in the HTML as a fallback for browsers without Intl support.
(function () {
  if (typeof Intl === 'undefined' || !Intl.DateTimeFormat) return;
  var format = new Intl.DateTimeFormat(undefined, {
    year: 'numeric', month: 'short', day: 'numeric',
    hour: 'numeric', minute: '2-digit', second: '2-digit', timeZoneName: 'short'
  });
  document.querySelectorAll('time[data-local-time]').forEach(function (element) {
    var date = new Date(element.getAttribute('datetime'));
    if (!isNaN(date.getTime())) {
      element.title = element.textContent;
      element.textContent = format.format(date);
    }
  });
}());
