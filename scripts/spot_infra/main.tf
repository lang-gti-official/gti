terraform {
  required_version = ">= 1.6, < 2.0"
  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 7.0"
    }
  }
}

provider "google" {
  project = var.project
  region  = local.region
}

variable "project" { type = string }
variable "zone" { type = string }
variable "bucket" { type = string }
variable "pool" { type = string }
variable "controller_account" { type = string }
variable "billing_account" { type = string }
variable "currency" { type = string }
variable "started" { type = string }
variable "pilot_expires_at" { type = string }
variable "janitor_image" {
  type = string
  validation {
    condition     = can(regex("^[a-z0-9.-]+/[a-zA-Z0-9_./-]+@sha256:[a-f0-9]{64}$", var.janitor_image))
    error_message = "Use an owner-published janitor image pinned by sha256 digest."
  }
}
variable "enable_scheduler" {
  type    = bool
  default = false
}

locals {
  region = join("-", slice(split("-", var.zone), 0, length(split("-", var.zone)) - 1))
  prefix = "gti-spot/v2/${var.pool}/"
  job    = "projects/${var.project}/locations/${local.region}/jobs/gti-spot-reaper"
  registry = {
    schema = 1, pool = var.pool, project = var.project, zone = var.zone,
    account = var.controller_account, billing_account = var.billing_account,
    currency = var.currency, started = var.started, pilot_expires_at = var.pilot_expires_at
  }
}

resource "google_project_service" "api" {
  for_each = toset(["compute.googleapis.com", "iap.googleapis.com", "oslogin.googleapis.com",
    "iam.googleapis.com", "iamcredentials.googleapis.com", "cloudbilling.googleapis.com", "run.googleapis.com", "cloudscheduler.googleapis.com",
    "storage.googleapis.com", "bigquery.googleapis.com", "cloudresourcemanager.googleapis.com"])
  service            = each.key
  disable_on_destroy = false
}

resource "google_compute_network" "runner" {
  name                    = "gti-spot-private"
  auto_create_subnetworks  = false
  depends_on              = [google_project_service.api]
}
resource "google_compute_subnetwork" "runner" {
  name                     = "runner-private"
  region                   = local.region
  ip_cidr_range            = "10.71.0.0/24"
  network                  = google_compute_network.runner.id
  private_ip_google_access = true
}
resource "google_compute_firewall" "iap" {
  name          = "gti-spot-iap-ssh"
  network       = google_compute_network.runner.name
  source_ranges = ["35.235.240.0/20"]
  allow {
    protocol = "tcp"
    ports    = ["22"]
  }
}

resource "google_storage_bucket" "recovery" {
  name                        = var.bucket
  location                    = local.region
  storage_class               = "STANDARD"
  uniform_bucket_level_access = true
  public_access_prevention    = "enforced"
  force_destroy               = false
  soft_delete_policy { retention_duration_seconds = 0 }
  lifecycle_rule {
    condition {
      age            = 1
      matches_prefix = ["${local.prefix}checkpoints/"]
    }
    action { type = "Delete" }
  }
  # Never expire leases/registry automatically: late insert and cost history
  # remain recoverable until the owner deliberately retires the pilot.
  depends_on = [google_project_service.api]
}
resource "google_storage_bucket_object" "registry" {
  name         = "gti-spot/v2/registry.json"
  bucket       = google_storage_bucket.recovery.name
  content      = jsonencode(local.registry)
  content_type = "application/json"
  lifecycle {
    prevent_destroy = true
    ignore_changes  = [content]
  }
}

resource "google_service_account" "upload" {
  account_id = "gti-spot-upload"
  depends_on = [google_project_service.api]
}
resource "google_service_account" "reaper" {
  account_id = "gti-spot-reaper"
  depends_on = [google_project_service.api]
}
resource "google_service_account" "scheduler" {
  account_id = "gti-spot-scheduler"
  depends_on = [google_project_service.api]
}
resource "google_storage_bucket_iam_member" "upload" {
  bucket = google_storage_bucket.recovery.name
  role   = "roles/storage.objectCreator"
  member = "serviceAccount:${google_service_account.upload.email}"
  condition {
    title      = "checkpoints-only"
    expression = "resource.name.startsWith('projects/_/buckets/${var.bucket}/objects/${local.prefix}checkpoints/')"
  }
}
resource "google_service_account_iam_member" "signer" {
  service_account_id = google_service_account.upload.name
  role               = "roles/iam.serviceAccountTokenCreator"
  member             = "user:${var.controller_account}"
}
resource "google_storage_bucket_iam_member" "controller_read" {
  bucket = google_storage_bucket.recovery.name
  role   = google_project_iam_custom_role.storage_read.name
  member = "user:${var.controller_account}"
}
resource "google_project_iam_custom_role" "storage_read" {
  role_id     = "gtiSpotStorageRead"
  title       = "GTI recovery bucket observation"
  permissions = ["storage.buckets.get", "storage.objects.get", "storage.objects.list"]
}
resource "google_storage_bucket_iam_member" "controller_lease" {
  bucket = google_storage_bucket.recovery.name
  role   = "roles/storage.objectCreator"
  member = "user:${var.controller_account}"
  condition {
    title      = "immutable-leases-only"
    expression = "resource.name.startsWith('projects/_/buckets/${var.bucket}/objects/${local.prefix}leases/')"
  }
}
resource "google_storage_bucket_iam_member" "reaper_read" {
  bucket = google_storage_bucket.recovery.name
  role   = "roles/storage.objectViewer"
  member = "serviceAccount:${google_service_account.reaper.email}"
}
resource "google_storage_bucket_iam_member" "reaper_receipts" {
  bucket = google_storage_bucket.recovery.name
  role   = "roles/storage.objectUser"
  member = "serviceAccount:${google_service_account.reaper.email}"
  condition {
    title = "receipts-and-heartbeat-only"
    expression = "resource.name == 'projects/_/buckets/${var.bucket}/objects/${local.prefix}health.json' || resource.name.startsWith('projects/_/buckets/${var.bucket}/objects/${local.prefix}receipts/')"
  }
}

resource "google_project_iam_custom_role" "controller" {
  role_id = "gtiSpotController"
  title   = "GTI single-project Spot controller"
  permissions = ["compute.instances.create", "compute.instances.get", "compute.instances.list", "compute.instances.delete",
    "compute.instances.setMetadata", "compute.instances.getGuestAttributes", "compute.instances.osAdminLogin",
    "compute.disks.create", "compute.disks.get", "compute.disks.list", "compute.disks.delete", "compute.disks.use",
    "compute.images.get", "compute.images.useReadOnly", "compute.subnetworks.get", "compute.subnetworks.use",
    "compute.networks.get", "compute.zoneOperations.get", "compute.projects.get",
    "resourcemanager.projects.get", "bigquery.jobs.create"]
}
resource "google_project_iam_member" "controller" {
  role   = google_project_iam_custom_role.controller.name
  member = "user:${var.controller_account}"
}
resource "google_project_iam_member" "controller_view" {
  for_each = toset(["roles/iap.tunnelResourceAccessor", "roles/run.viewer", "roles/cloudscheduler.viewer"])
  role     = each.key
  member   = "user:${var.controller_account}"
}
resource "google_project_iam_custom_role" "reaper_read" {
  role_id     = "gtiSpotReaperRead"
  title       = "GTI Spot cleanup inventory"
  permissions = ["compute.instances.get", "compute.instances.list", "compute.disks.get", "compute.disks.list", "compute.zoneOperations.get"]
}
resource "google_project_iam_custom_role" "reaper_delete" {
  role_id     = "gtiSpotReaperDelete"
  title       = "GTI Spot cleanup deletion"
  permissions = ["compute.instances.delete", "compute.disks.delete"]
}
resource "google_project_iam_member" "reaper_read" {
  role   = google_project_iam_custom_role.reaper_read.name
  member = "serviceAccount:${google_service_account.reaper.email}"
}
resource "google_project_iam_member" "reaper_delete" {
  role   = google_project_iam_custom_role.reaper_delete.name
  member = "serviceAccount:${google_service_account.reaper.email}"
  condition {
    title = "runner-prefix-and-zone-only"
    expression = "resource.name.startsWith('projects/${var.project}/zones/${var.zone}/instances/gti-spot-') || resource.name.startsWith('projects/${var.project}/zones/${var.zone}/disks/gti-spot-')"
  }
}
resource "google_project_iam_custom_role" "pause" {
  role_id     = "gtiSpotPauseScheduler"
  title       = "Expire GTI Spot schedule"
  permissions = ["cloudscheduler.jobs.pause"]
}
resource "google_project_iam_member" "pause" {
  role   = google_project_iam_custom_role.pause.name
  member = "serviceAccount:${google_service_account.reaper.email}"
  condition {
    title      = "one-schedule-only"
    expression = "resource.name == '${local.job}'"
  }
}

resource "google_cloud_run_v2_job" "reaper" {
  name                = "gti-spot-reaper"
  location            = local.region
  deletion_protection = false
  template {
    task_count  = 1
    parallelism = 1
    template {
      service_account = google_service_account.reaper.email
      max_retries     = 0
      timeout         = "60s"
      containers {
        image = var.janitor_image
        args = ["--project", var.project, "--bucket", var.bucket, "--pool", var.pool,
          "--expires", var.pilot_expires_at, "--scheduler", local.job]
        resources { limits = { cpu = "1", memory = "512Mi" } }
      }
    }
  }
  depends_on = [google_project_service.api]
}
resource "google_cloud_run_v2_job_iam_member" "scheduler" {
  name     = google_cloud_run_v2_job.reaper.name
  location = local.region
  role     = "roles/run.invoker"
  member   = "serviceAccount:${google_service_account.scheduler.email}"
}
resource "google_cloud_run_v2_job_iam_member" "controller" {
  name     = google_cloud_run_v2_job.reaper.name
  location = local.region
  role     = "roles/run.invoker"
  member   = "user:${var.controller_account}"
}
resource "google_cloud_scheduler_job" "reaper" {
  name             = "gti-spot-reaper"
  region           = local.region
  schedule         = "*/10 * * * *"
  time_zone        = "Etc/UTC"
  paused           = !var.enable_scheduler
  attempt_deadline = "60s"
  retry_config { retry_count = 0 }
  http_target {
    uri         = "https://run.googleapis.com/v2/${local.job}:run"
    http_method = "POST"
    body        = base64encode("{}")
    headers     = { "Content-Type" = "application/json" }
    oauth_token { service_account_email = google_service_account.scheduler.email }
  }
  lifecycle {
    precondition {
      condition     = !var.enable_scheduler || (timecmp(var.pilot_expires_at, timeadd(timestamp(), "168h")) <= 0 && timecmp(var.pilot_expires_at, timestamp()) > 0)
      error_message = "Enabling recurring cleanup requires a pilot expiry in the next seven days."
    }
  }
  depends_on = [google_cloud_run_v2_job_iam_member.scheduler]
}

output "recovery_registry" { value = jsondecode(google_storage_bucket_object.registry.content) }
output "subnetwork" { value = google_compute_subnetwork.runner.id }
output "teardown_note" {
  value = "Pause the schedule, reconcile all VMs/disks and delayed operations, retain receipts, delete the versioned worker image, and deliberately retire the protected registry/bucket. Storage and images incur costs until removed."
}
