// Copyright (c) Yugabyte, Inc.
package com.yugabyte.yw.models;

import java.util.HashMap;
import java.util.Map;
import java.util.List;
import java.util.Set;
import java.util.UUID;

import javax.persistence.CascadeType;
import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;
import javax.persistence.ManyToOne;
import javax.persistence.OneToMany;

import com.avaje.ebean.*;
import com.avaje.ebean.annotation.DbJson;
import com.fasterxml.jackson.annotation.JsonBackReference;
import com.fasterxml.jackson.annotation.JsonIgnore;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import play.data.validation.Constraints;
import play.libs.Json;

import static com.avaje.ebean.Ebean.beginTransaction;
import static com.avaje.ebean.Ebean.commitTransaction;
import static com.avaje.ebean.Ebean.endTransaction;
import static com.yugabyte.yw.models.helpers.CommonUtils.maskConfig;

@Entity
public class Region extends Model {
  private static final String SECURITY_GROUP_KEY = "sg_id";

  @Id
  public UUID uuid;

  @Column(length = 25, nullable = false)
  public String code;

  @Column(length = 100, nullable = false)
  @Constraints.Required
  public String name;

  // The AMI to be used in this region.
  @Constraints.Required
  public String ybImage;

  @Column(columnDefinition = "float")
  public double longitude;

  @Column(columnDefinition = "float")
  public double latitude;

  public void setLatLon(double latitude, double longitude) {
    if (latitude < -90 || latitude > 90) {
      throw new IllegalArgumentException("Invalid Latitude Value, it should be between -90 to 90");
    }
    if (longitude < -180 || longitude > 180) {
      throw new IllegalArgumentException("Invalid Longitude Value, it should be between -180 to 180");
    }

    this.latitude = latitude;
    this.longitude = longitude;
    this.save();
  }

  @Constraints.Required
  @Column(nullable = false)
  @ManyToOne
  @JsonBackReference
  public Provider provider;

  @OneToMany(cascade=CascadeType.ALL)
  public Set<AvailabilityZone> zones;

  @Column(nullable = false, columnDefinition = "boolean default true")
  public Boolean active = true;
  public Boolean isActive() { return active; }
  public void setActiveFlag(Boolean active) { this.active = active; }

  @DbJson
  @Column(columnDefinition = "TEXT")
  public JsonNode details;

  public void setSecurityGroupId(String securityGroupId) {
    if (details == null) {
      details = Json.newObject();
    }
    ((ObjectNode) details).put(SECURITY_GROUP_KEY, securityGroupId);
    save();
  }

  public String getSecurityGroupId() {
    if (details != null) {
      JsonNode sgNode = details.get(SECURITY_GROUP_KEY);
      return sgNode == null || sgNode.isNull() ? null : sgNode.asText();
    }
    return null;
  }

  @DbJson
  @Column(columnDefinition = "TEXT")
  public JsonNode config;

  public void setConfig(Map<String, String> configMap) {
    Map<String, String> currConfig = this.getConfig();
    for (String key : configMap.keySet()) {
      currConfig.put(key, configMap.get(key));
    }
    this.config = Json.toJson(currConfig);
    this.save();
  }

  @JsonIgnore
  public JsonNode getMaskedConfig() {
    if (this.config == null) {
      return Json.newObject();
    } else {
      return maskConfig(this.config);
    }
  }

  @JsonIgnore
  public Map<String, String> getConfig() {
    if (this.config == null) {
      return new HashMap();
    } else {
      return Json.fromJson(this.config, Map.class);
    }
  }

  /**
   * Query Helper for PlacementRegion with region code
   */
  public static final Find<UUID, Region> find = new Find<UUID, Region>(){};

  /**
   * Create new instance of PlacementRegion
   * @param provider Cloud Provider
   * @param code Unique PlacementRegion Code
   * @param name User Friendly PlacementRegion Name
   * @param ybImage The YB image id that we need to use for provisioning in this region
   * @return instance of PlacementRegion
   */
  public static Region create(Provider provider, String code, String name, String ybImage) {
    return create(provider, code, name, ybImage, 0.0, 0.0);
  }
  // Overload create function with lat, long values for OnPrem case
  public static Region create(Provider provider, String code, String name, String ybImage, double latitude, double longitude) {
    Region region = new Region();
    region.provider = provider;
    region.code = code;
    region.name = name;
    region.ybImage = ybImage;
    region.latitude = latitude;
    region.longitude = longitude;
    region.save();
    return region;
  }

  public static Region createWithMetadata(Provider provider, String code, JsonNode metadata) {
    Region region = Json.fromJson(metadata, Region.class);
    region.provider = provider;
    region.code = code;
    region.save();
    return region;
  }

  public static Region get(UUID regionUUID) {
    return find.fetch("provider").where().idEq(regionUUID).findUnique();
  }

  public static Region getByCode(Provider provider, String code) {
    return find.where().eq("provider_uuid", provider.uuid).eq("code", code).findUnique();
  }

  public static List<Region> getByProvider(UUID providerUUID) {
    return find.where().eq("provider_uuid", providerUUID).findList();
  }

  public static Region get(UUID customerUUID, UUID providerUUID, UUID regionUUID) {
    String regionQuery
        = " select r.uuid, r.code, r.name"
        + "   from region r join provider p on p.uuid = r.provider_uuid "
        + "  where r.uuid = :r_uuid and p.uuid = :p_uuid and p.customer_uuid = :c_uuid";

    RawSql rawSql = RawSqlBuilder.parse(regionQuery).create();
    Query<Region> query = Ebean.find(Region.class);
    query.setRawSql(rawSql);
    query.setParameter("r_uuid", regionUUID);
    query.setParameter("p_uuid", providerUUID);
    query.setParameter("c_uuid", customerUUID);
    return query.findUnique();
  }

  /**
   * Fetch Regions with the minimum zone count and having a valid yb server image.
   * @param customerUUID
   * @param providerUUID
   * @param minZoneCount
   * @return List of PlacementRegion
   */
  public static List<Region> fetchValidRegions(UUID customerUUID, UUID providerUUID, int minZoneCount) {
    String regionQuery
      = " select r.uuid, r.code, r.name"
      + "   from region r join provider p on p.uuid = r.provider_uuid "
      + "   left outer join availability_zone zone on zone.region_uuid = r.uuid "
      + "  where p.uuid = :p_uuid and p.customer_uuid = :c_uuid"
      + "  group by r.uuid "
      + " having count(zone.uuid) >= " + minZoneCount;

    RawSql rawSql = RawSqlBuilder.parse(regionQuery).create();
    Query<Region> query = Ebean.find(Region.class);
    query.setRawSql(rawSql);
    query.setParameter("p_uuid", providerUUID);
    query.setParameter("c_uuid", customerUUID);
    return query.findList();
  }

  public void disableRegionAndZones() {
    beginTransaction();
    try {
      setActiveFlag(false);
      update();
      String s = "UPDATE availability_zone set active = :active_flag where region_uuid = :region_uuid";
      SqlUpdate updateStmt = Ebean.createSqlUpdate(s);
      updateStmt.setParameter("active_flag", false);
      updateStmt.setParameter("region_uuid", uuid);
      Ebean.execute(updateStmt);
      commitTransaction();
    } catch (Exception e) {
      throw new RuntimeException("Unable to flag Region UUID as deleted: " + uuid);
    } finally {
      endTransaction();
    }
  }
}
