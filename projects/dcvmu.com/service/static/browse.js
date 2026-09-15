// Keep the GET form usable without JavaScript; dropdowns apply immediately when available.
(function () {
  var form = document.getElementById('browse-filters');
  if (!form) return;
  form.querySelectorAll('[data-auto-apply]').forEach(function (select) {
    select.addEventListener('change', function () {
      if (form.requestSubmit) form.requestSubmit();
      else form.submit();
    });
  });
}());
