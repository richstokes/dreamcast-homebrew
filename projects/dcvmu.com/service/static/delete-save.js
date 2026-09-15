"use strict";
var deleteLink = document.getElementById("delete-save-link");
var deleteForm = document.getElementById("delete-save-form");
if (deleteLink && deleteForm) {
    deleteLink.addEventListener("click", function (event) {
        event.preventDefault();
        var name = document.querySelector("h1").textContent;
        if (window.confirm('Permanently delete "' + name + '" from your account? This cannot be undone.')) {
            deleteForm.submit();
        }
    });
}
