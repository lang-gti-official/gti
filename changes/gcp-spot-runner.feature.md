Development automation adds an opt-in, single-worker Google Spot runner around
the existing remote executor. Disabled-by-default owner policy, durable cost
reservations, provider-enforced expiry/deletion and private IAP SSH keep the
pilot bounded. Live cloud commissioning and owner-reviewed pricing/billing
configuration are required before use; existing runner defaults do not change.
